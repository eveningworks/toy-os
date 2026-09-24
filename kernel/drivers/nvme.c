// NVM Express: a PCIe SSD's own protocol, with no ATA underneath.
//
// THE SHAPE. A controller has an ADMIN queue pair (identify, create
// queues, features) and any number of I/O queue pairs; a command is a
// 64-byte entry written into a submission queue and announced by a
// doorbell write, and its answer is a 16-byte entry the controller
// writes into the paired completion queue, told apart from stale ones by
// a PHASE bit that flips each time the queue wraps. Linux's nvme-pci
// makes one I/O queue pair per CPU with an MSI-X vector each; with one
// CPU online this makes ONE, on MSI-X entry 0 (pci_msi.c programs no
// other) -- the same shape, sized to the machine, and the admin queue
// shares the vector as the spec allows.
//
// WHAT IS DELIBERATELY NOT HERE: a second I/O queue (nothing to run it
// on until docs/smp-design.md lands), namespace management, a shutdown
// notification on power-off, and error recovery beyond "disable the
// controller" -- see nvme_fail().
//
// DMA GOES STRAIGHT INTO THE CALLER'S BUFFER. Kernel memory is identity-
// mapped, so a virtually contiguous buffer is physically contiguous and
// its PRP entries are consecutive pages (virtio-blk relies on the same
// fact). A buffer that is not dword-aligned or ends above the identity
// map goes through a bounce buffer instead.
#include "nvme.h"
#include "block.h"
#include "pci.h"
#include "pci_internal.h"
#include "pci_driver.h"
#include "paging.h"
#include "pmm.h"
#include "irq.h"
#include "idt.h"
#include "kmutex.h"
#include "scheduler.h"
#include "clocksource.h"
#include "barrier.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`

DRIVER_DECLARE("nvme", "block", "NVM Express SSD");

// ---- registers (NVMe 1.4, 3.1) ------------------------------------------

#define REG_CAP   0x00   // u64
#define REG_VS    0x08
#define REG_INTMS 0x0C
#define REG_INTMC 0x10
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28   // u64
#define REG_ACQ   0x30   // u64
#define REG_DOORBELLS 0x1000

#define CC_EN          (1u << 0)
#define CC_IOSQES(n)   ((uint32_t)(n) << 16)   // log2 of a submission entry: 6
#define CC_IOCQES(n)   ((uint32_t)(n) << 20)   // log2 of a completion entry: 4
#define CSTS_RDY       (1u << 0)
#define CSTS_CFS       (1u << 1)

// Admin opcodes.
#define ADM_CREATE_SQ  0x01
#define ADM_CREATE_CQ  0x05
#define ADM_IDENTIFY   0x06
#define ADM_SET_FEAT   0x09
// NVM opcodes.
#define NVM_FLUSH      0x00
#define NVM_WRITE      0x01
#define NVM_READ       0x02
#define NVM_DSM        0x09

#define FEAT_NUM_QUEUES 0x07

// ---- queue geometry ------------------------------------------------------

#define ADMIN_DEPTH 16
#define IO_DEPTH    32      // entries; one is always empty, so 31 in flight
#define IO_INFLIGHT (IO_DEPTH - 1)
#define PAGE        4096u

// One contiguous DMA32 run, laid out by fixed offsets so there is no
// alignment arithmetic to get wrong. EVERY QUEUE STARTS A PAGE: ASQ and
// ACQ have their low 12 bits reserved, and a contiguous I/O queue's PRP1
// must be page-aligned -- QEMU refuses to enable a controller whose ACQ
// sat half-way into a page, which is how this layout was found.
#define OFF_ASQ     0x0000   // 16 x 64 B
#define OFF_ACQ     0x1000   // 16 x 16 B
#define OFF_IOSQ    0x2000   // 32 x 64 B
#define OFF_IOCQ    0x3000   // 32 x 16 B
#define OFF_SCRATCH 0x4000   // identify data, a DSM range list
#define OFF_PRP     0x5000   // one PRP list page per command id
#define DMA_FRAMES  (5 + IO_INFLIGHT)

#define BOUNCE_FRAMES 64     // 256 KiB, stepped down if the pool is fragmented

struct nvme_sqe {
    uint8_t  opc, flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
_Static_assert(sizeof(struct nvme_sqe) == 64, "submission entry is 64 bytes");

struct nvme_cqe {
    uint32_t dw0, dw1;
    uint16_t sqhd, sqid;
    uint16_t cid;
    uint16_t status;     // bit 0 the phase tag, 15:1 the status field
};
_Static_assert(sizeof(struct nvme_cqe) == 16, "completion entry is 16 bytes");

struct nvme_queue {
    volatile struct nvme_sqe *sq;
    volatile struct nvme_cqe *cq;
    volatile uint32_t *sq_db, *cq_db;
    uint16_t depth, sq_tail, cq_head;
    uint8_t phase;
    // Per command id: filled by reap(), read by the waiter.
    volatile uint8_t done[IO_DEPTH];
    volatile uint16_t status[IO_DEPTH];
    volatile uint32_t dw0[IO_DEPTH];
};

struct nvme_ns {
    uint32_t nsid;
    uint32_t block_size;    // 512 or 4096
    uint32_t spb;           // 512-byte sectors per block
    uint32_t sectors;       // capacity in 512-byte sectors, clamped to 32 bits
};

static const struct pci_device *g_pci;
static volatile uint8_t *g_regs;
static uint32_t g_dstrd;          // doorbell stride, bytes
static uint64_t g_dma;            // the fixed-layout run above
static uint8_t *g_bounce;
static uint32_t g_bounce_bytes;
static uint32_t g_max_bytes;      // one transfer, after MDTS and the bounce
static struct nvme_queue g_admin, g_io;
static struct nvme_ns g_ns[NVME_MAX_NS];
static int g_ns_count;
static int g_vwc, g_dsm;
static char g_model[41];
static int g_dead;                // a timeout or a fatal status disabled it

static uint8_t g_irq;             // INTx line, 0 when not on one
static uint8_t g_msi_vector;      // LAPIC vector, 0 when not on one
static const char g_chan;         // what a waiter parks on
static uint64_t g_irqs, g_sleeps;

// ONE OPERATION AT A TIME on the controller: one command, or one batch.
// Taken by every public entry; every namespace shares it, as they share
// the queue.
static struct kmutex g_lock;

static uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(g_regs + off); }
static void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_regs + off) = v; }
static uint64_t rd64(uint32_t off) { return (uint64_t)rd32(off) | ((uint64_t)rd32(off + 4) << 32); }
static void wr64(uint32_t off, uint64_t v) { wr32(off, (uint32_t)v); wr32(off + 4, (uint32_t)(v >> 32)); }

static volatile uint32_t *doorbell(int qid, int cq) {
    return (volatile uint32_t *)(g_regs + REG_DOORBELLS + (2u * (uint32_t)qid + (uint32_t)cq) * g_dstrd);
}

static void *dma_at(uint32_t off) { return (void *)(uintptr_t)(g_dma + off); }

// ---- completions --------------------------------------------------------

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
}

// Consumes every new completion on `q`: records its status against its
// command id, advances the head, and tells the controller how far it got.
// Called from BOTH the interrupt handler and a waiter, so a waiter runs it
// with interrupts off -- one CPU, so that is all the exclusion needed.
// Returns how many it took.
static int reap_locked(struct nvme_queue *q) {
    int n = 0;
    for (;;) {
        volatile struct nvme_cqe *e = &q->cq[q->cq_head];
        uint16_t st = e->status;
        if ((st & 1u) != q->phase) break;
        kmb();                                   // the entry before its fields
        uint16_t cid = e->cid;
        if (cid < q->depth) {
            q->status[cid] = (uint16_t)(st >> 1);
            q->dw0[cid] = e->dw0;
            q->done[cid] = 1;
        }
        if (++q->cq_head == q->depth) { q->cq_head = 0; q->phase ^= 1; }
        n++;
    }
    if (n) *q->cq_db = q->cq_head;
    return n;
}

static int reap(struct nvme_queue *q) {
    uint64_t f = irq_save();
    int n = reap_locked(q);
    irq_restore(f);
    return n;
}

static void irq_handler(uint64_t *regs) {
    (void)regs;
    if (!g_regs || !g_admin.cq) return;
    // A shared INTx line may not be ours; an MSI-X vector always is, and
    // is COUNTED even when a polling waiter reaped first -- the count is
    // the evidence the vector delivers, which a poll cannot fake.
    if (g_msi_vector) g_irqs++;
    int n = reap_locked(&g_admin) + (g_io.cq ? reap_locked(&g_io) : 0);
    if (!n) return;
    if (!g_msi_vector) g_irqs++;
    scheduler_wake(&g_chan, 0);   // interrupt-safe, by contract
}

// ---- waiting ------------------------------------------------------------

#define TIMEOUT_NS  (10ull * 1000000000ull)
// Iterations of a paused poll: the backstop when the clock cannot be
// trusted to move (interrupts off), sized as virtqueue.c's.
#define POLL_LIMIT  20000000ull

static int all_done(const struct nvme_queue *q, int n) {
    for (int i = 0; i < n; i++) if (!q->done[i]) return 0;
    return 1;
}

// Waits for command ids 0 .. n-1 on `q`. A scheduled caller with an
// interrupt SLEEPS until the handler wakes it; anything else polls the
// completion queue. 1 when all are done, 0 on a timeout.
static int wait_for(struct nvme_queue *q, int n) {
    uint64_t deadline = clocksource_now_ns() + TIMEOUT_NS;
    if ((g_irq || g_msi_vector) && !isr_in_progress()) {
        for (;;) {
            scheduler_wait_arm(&g_chan);
            reap(q);
            if (all_done(q, n)) { scheduler_wait_disarm(); return 1; }
            if (clocksource_now_ns() >= deadline) { scheduler_wait_disarm(); return 0; }
            if (!scheduler_block_kernel_until(&g_chan, SCHED_WAIT_DISK, deadline)) {
                scheduler_wait_disarm();
                break;                          // nowhere to park: poll
            }
            g_sleeps++;
        }
    }
    for (uint64_t i = 0; i < POLL_LIMIT; i++) {
        reap(q);
        if (all_done(q, n)) return 1;
        if ((i & 0x3FF) == 0 && clocksource_now_ns() >= deadline) return 0;
        cpu_relax();
    }
    return 0;
}

// A command that never completed may still DMA into its buffer later,
// and that buffer is about to be handed back to its caller. Disabling
// the controller is the one thing that stops it (CC.EN=0 is a reset),
// so that is what a timeout costs: every namespace goes with it, loudly.
// Linux aborts the command first and resets only if that fails too.
static void nvme_fail(const char *why) {
    if (g_dead) return;
    g_dead = 1;
    klog_printf(KLOG_ERR "nvme: %s -- controller disabled, its disks are gone until reboot\n", why);
    wr32(REG_CC, rd32(REG_CC) & ~CC_EN);
}

// Submits `n` prepared entries on `q` (command ids 0 .. n-1) and waits.
// 1 when every one completed, whatever its status; the caller reads
// q->status[].
static int submit_and_wait(struct nvme_queue *q, struct nvme_sqe *cmds, int n) {
    if (g_dead) return 0;
    for (int i = 0; i < n; i++) {
        cmds[i].cid = (uint16_t)i;
        q->done[i] = 0;
        q->status[i] = 0xFFFF;
        q->sq[q->sq_tail] = cmds[i];
        if (++q->sq_tail == q->depth) q->sq_tail = 0;
    }
    kmb();                                      // the entries before the doorbell
    *q->sq_db = q->sq_tail;
    if (!wait_for(q, n)) {
        uint32_t csts = rd32(REG_CSTS);
        char why[64];
        k_snprintf(why, sizeof why, "command 0x%x timed out (csts 0x%x)", cmds[0].opc, csts);
        nvme_fail(why);
        return 0;
    }
    if (rd32(REG_CSTS) & CSTS_CFS) { nvme_fail("controller fatal status"); return 0; }
    return 1;
}

static int admin(struct nvme_sqe *cmd, uint32_t *dw0) {
    if (!submit_and_wait(&g_admin, cmd, 1)) return 0;
    if (g_admin.status[0]) {
        klog_printf("nvme: admin command 0x%x failed, status 0x%x\n", cmd->opc, g_admin.status[0]);
        return 0;
    }
    if (dw0) *dw0 = g_admin.dw0[0];
    return 1;
}

// ---- PRPs ---------------------------------------------------------------

// Fills prp1/prp2 for `bytes` at physical `phys`, using command id
// `cid`'s list page when more than two pages are touched. PRP1 may carry
// an offset into its page; every later entry is a whole page.
static void set_prps(struct nvme_sqe *c, int cid, uint64_t phys, uint32_t bytes) {
    uint64_t first = phys & ~(uint64_t)(PAGE - 1);
    uint32_t pages = (uint32_t)(((phys - first) + bytes + PAGE - 1) / PAGE);
    c->prp1 = phys;
    c->prp2 = 0;
    if (pages == 2) {
        c->prp2 = first + PAGE;
    } else if (pages > 2) {
        uint64_t *list = dma_at(OFF_PRP + (uint32_t)cid * PAGE);
        for (uint32_t k = 1; k < pages; k++) list[k - 1] = first + (uint64_t)k * PAGE;
        c->prp2 = g_dma + OFF_PRP + (uint32_t)cid * PAGE;
    }
}

// ---- identify -----------------------------------------------------------

static int identify(uint32_t cns, uint32_t nsid) {
    struct nvme_sqe c = { .opc = ADM_IDENTIFY, .nsid = nsid, .cdw10 = cns };
    c.prp1 = g_dma + OFF_SCRATCH;
    k_memset(dma_at(OFF_SCRATCH), 0, PAGE);
    return admin(&c, 0);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_ascii(char *out, const uint8_t *in, int n) {
    for (int i = 0; i < n; i++) out[i] = (in[i] >= 0x20 && in[i] < 0x7F) ? (char)in[i] : ' ';
    out[n] = 0;
    for (int i = n - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

// One namespace: its size and block format. A format this kernel cannot
// address is REFUSED, not guessed at: a block size other than 512 or
// 4096, or metadata interleaved with the data, which would put the
// driver's byte counts out of step with the device's.
static void add_namespace(uint32_t nsid) {
    if (g_ns_count >= NVME_MAX_NS) {
        klog_printf("nvme: namespace %u ignored -- %d are driven\n", nsid, NVME_MAX_NS);
        return;
    }
    if (!identify(0, nsid)) return;
    const uint8_t *d = dma_at(OFF_SCRATCH);
    uint64_t nsze = (uint64_t)le32(d) | ((uint64_t)le32(d + 4) << 32);
    if (!nsze) return;                               // allocated but not attached
    uint32_t fmt = d[26] & 0x0F;
    const uint8_t *lbaf = d + 128 + fmt * 4;
    uint32_t ms = (uint32_t)lbaf[0] | ((uint32_t)lbaf[1] << 8);
    uint32_t lbads = lbaf[2];
    if ((lbads != 9 && lbads != 12) || ms) {
        klog_printf("nvme: namespace %u refused -- %u-byte blocks with %u bytes of metadata\n",
                    nsid, 1u << lbads, ms);
        return;
    }
    struct nvme_ns *ns = &g_ns[g_ns_count];
    ns->nsid = nsid;
    ns->block_size = 1u << lbads;
    ns->spb = ns->block_size / 512;
    uint64_t sectors = nsze * ns->spb;
    if (sectors > 0xFFFFFFFFull) {
        klog_printf("nvme: namespace %u clamped to 2 TiB (32-bit block layer)\n", nsid);
        sectors = 0xFFFFFFFFull & ~(uint64_t)(ns->spb - 1);
    }
    ns->sectors = (uint32_t)sectors;
    klog_printf("nvme: namespace %u: %u sectors, %u-byte blocks\n",
                nsid, ns->sectors, ns->block_size);
    g_ns_count++;
}

// The active namespace list (CNS 2) where the controller has one, else
// every NSID up to NN -- a 1.0 controller predates the list.
static void scan_namespaces(uint32_t nn) {
    if (identify(2, 0)) {
        static uint32_t ids[64];   // add_namespace() reuses the scratch page
        int k = 0;
        const uint8_t *d = dma_at(OFF_SCRATCH);
        for (; k < 64; k++) {
            ids[k] = le32(d + 4 * k);
            if (!ids[k]) break;
        }
        for (int i = 0; i < k; i++) add_namespace(ids[i]);
        return;
    }
    for (uint32_t id = 1; id <= nn && id <= 64; id++) add_namespace(id);
}

// ---- bring-up -----------------------------------------------------------

static int wait_ready(int want, uint64_t cap) {
    uint64_t ms = ((cap >> 24) & 0xFF) * 500;          // CAP.TO, 500 ms units
    if (!ms) ms = 500;
    uint64_t deadline = clocksource_now_ns() + ms * 1000000ull;
    for (uint64_t i = 0; i < POLL_LIMIT * 4; i++) {
        uint32_t csts = rd32(REG_CSTS);
        if (csts & CSTS_CFS) return 0;
        if ((int)(csts & CSTS_RDY) == want) return 1;
        if ((i & 0x3FF) == 0 && clocksource_now_ns() >= deadline) return 0;
        cpu_relax();
    }
    return 0;
}

static void queue_init(struct nvme_queue *q, uint32_t sq_off, uint32_t cq_off, int qid, int depth) {
    k_memset(q, 0, sizeof *q);
    q->sq = dma_at(sq_off);
    q->cq = dma_at(cq_off);
    q->sq_db = doorbell(qid, 0);
    q->cq_db = doorbell(qid, 1);
    q->depth = (uint16_t)depth;
    q->phase = 1;
}

static int alloc_dma(void) {
    g_dma = pmm_alloc_contiguous(DMA_FRAMES, PMM_ZONE_DMA32);
    if (!g_dma) return 0;
    k_memset(dma_at(0), 0, DMA_FRAMES * PAGE);
    static const uint32_t TRY[] = { BOUNCE_FRAMES, 16, 1 };
    for (unsigned i = 0; i < sizeof TRY / sizeof TRY[0]; i++) {
        uint64_t b = pmm_alloc_contiguous(TRY[i], PMM_ZONE_DMA32);
        if (b) { g_bounce = (uint8_t *)(uintptr_t)b; g_bounce_bytes = TRY[i] * PAGE; return 1; }
    }
    return 0;
}

static int bring_up(uint64_t cap) {
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
    if (!(cap & (1ull << 37))) { klog_write("nvme: controller lacks the NVM command set\n"); return 0; }
    if (((cap >> 48) & 0xF) != 0) { klog_write("nvme: controller refuses 4 KiB pages\n"); return 0; }
    if (mqes < IO_DEPTH) { klog_printf("nvme: queues limited to %u entries\n", mqes); return 0; }
    g_dstrd = 4u << ((cap >> 32) & 0xF);

    // Disabled first: firmware may have left it running, and the admin
    // queue registers are only written while it is off.
    if (rd32(REG_CC) & CC_EN) wr32(REG_CC, rd32(REG_CC) & ~CC_EN);
    if (!wait_ready(0, cap)) { klog_write("nvme: controller would not disable\n"); return 0; }

    queue_init(&g_admin, OFF_ASQ, OFF_ACQ, 0, ADMIN_DEPTH);
    wr32(REG_AQA, ((ADMIN_DEPTH - 1u) << 16) | (ADMIN_DEPTH - 1u));
    wr64(REG_ASQ, g_dma + OFF_ASQ);
    wr64(REG_ACQ, g_dma + OFF_ACQ);
    wr32(REG_CC, CC_EN | CC_IOSQES(6) | CC_IOCQES(4));   // NVM set, 4 KiB pages, round robin
    if (!wait_ready(1, cap)) { klog_write("nvme: controller would not enable\n"); return 0; }
    return 1;
}

// One I/O queue pair, on vector 0 with the admin queue.
static int create_io_queues(void) {
    struct nvme_sqe c = { .opc = ADM_SET_FEAT, .cdw10 = FEAT_NUM_QUEUES, .cdw11 = 0 };
    if (!admin(&c, 0)) return 0;

    queue_init(&g_io, OFF_IOSQ, OFF_IOCQ, 1, IO_DEPTH);
    struct nvme_sqe cq = { .opc = ADM_CREATE_CQ, .prp1 = g_dma + OFF_IOCQ,
                           .cdw10 = ((IO_DEPTH - 1u) << 16) | 1u,
                           .cdw11 = (1u << 1) | 1u };             // interrupts on, contiguous
    if (!admin(&cq, 0)) return 0;
    struct nvme_sqe sq = { .opc = ADM_CREATE_SQ, .prp1 = g_dma + OFF_IOSQ,
                           .cdw10 = ((IO_DEPTH - 1u) << 16) | 1u,
                           .cdw11 = (1u << 16) | 1u };            // on CQ 1, contiguous
    return admin(&sq, 0);
}

static void nvme_probe(const struct pci_device *dev) {
    if (g_pci) {
        klog_printf("nvme: a second controller at %02x:%02x.%u -- one is driven\n",
                    dev->bus, dev->device, dev->function);
        return;
    }
    g_pci = dev;
    uint64_t bar = pci_bar_mem_addr(dev, 0);
    uint64_t size = pci_bar_mem_size(dev, 0);
    if (!bar || size < 0x2000) { klog_write("nvme: BAR0 is missing or too small\n"); return; }
    g_regs = (volatile uint8_t *)paging_map_device(bar, size < 0x4000 ? size : 0x4000);
    if (!g_regs) { klog_printf(KLOG_ERR "nvme: BAR0 at 0x%llx could not be mapped\n", (unsigned long long)bar); return; }

    // Bus mastering on, the pin off until an interrupt is chosen below.
    pci_command_update(dev, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE, 0);

    uint64_t cap = rd64(REG_CAP);
    uint32_t vs = rd32(REG_VS);
    if (!alloc_dma()) { klog_write("nvme: out of contiguous DMA memory\n"); g_regs = 0; return; }
    if (!bring_up(cap) || !create_io_queues()) { nvme_fail("bring-up failed"); return; }

    if (!identify(1, 0)) { nvme_fail("IDENTIFY CONTROLLER failed"); return; }
    const uint8_t *d = dma_at(OFF_SCRATCH);
    copy_ascii(g_model, d + 24, 40);
    uint32_t mdts = d[77];
    uint32_t nn = le32(d + 516);
    g_dsm = (d[520] & (1u << 2)) != 0;
    g_vwc = (d[525] & 1u) != 0;
    g_max_bytes = g_bounce_bytes;
    if (mdts && mdts < 20 && (PAGE << mdts) < g_max_bytes) g_max_bytes = PAGE << mdts;
    // A PRP list page holds 512 entries; 256 KiB needs 64. Not a limit
    // this reaches, stated so a larger bounce buffer does not walk past it.
    if (g_max_bytes > 512 * PAGE) g_max_bytes = 512 * PAGE;

    scan_namespaces(nn);

    // THE COMMIT POINT: everything the handler touches exists. MSI-X
    // first (pci_msi_request() tries it, then MSI), else the pin, else
    // every wait polls.
    g_msi_vector = pci_msi_request(dev, irq_handler);
    if (!g_msi_vector) {
        uint8_t line = pci_irq_line(dev);
        if (line != IRQ_NONE) {
            g_irq = line;
            irq_register_handler(g_irq, irq_handler);
            pci_command_update(dev, 0, PCI_CMD_INTX_DISABLE);
            irq_unmask(g_irq);
        }
    }

    klog_printf("nvme: \"%s\", NVMe %u.%u, %d namespace%s, %u KiB/transfer, flush %s, trim %s, %s\n",
                g_model, vs >> 16, (vs >> 8) & 0xFF, g_ns_count, g_ns_count == 1 ? "" : "s",
                g_max_bytes / 1024, g_vwc ? "yes" : "no (no volatile cache)",
                g_dsm ? "yes" : "no",
                g_msi_vector ? (dev->irq_msix ? "MSI-X" : "MSI") : g_irq ? "INTx" : "polled");
}

static const struct pci_match nvme_matches[] = {
    PCI_MATCH_CLASS(0x01, 0x08, 0x02),
};
PCI_DRIVER("nvme", nvme_matches, nvme_probe);

// ---- transfers ----------------------------------------------------------

int nvme_ns_count(void) { return g_dead ? 0 : g_ns_count; }
uint32_t nvme_ns_id(int ns) { return ns >= 0 && ns < g_ns_count ? g_ns[ns].nsid : 0; }
uint32_t nvme_ns_sector_count(int ns) { return ns >= 0 && ns < g_ns_count ? g_ns[ns].sectors : 0; }
uint32_t nvme_ns_block_size(int ns) { return ns >= 0 && ns < g_ns_count ? g_ns[ns].block_size : 512; }
int nvme_max_sectors_per_xfer(void) { return (int)(g_max_bytes / 512); }
int nvme_has_flush(void) { return g_vwc; }
int nvme_has_trim(void) { return g_dsm; }
int nvme_irq_driven(void) { return g_irq || g_msi_vector; }
uint64_t nvme_irq_count(void) { return g_irqs; }
uint64_t nvme_sleeps(void) { return g_sleeps; }
const char *nvme_model(void) { return g_model; }

// Refused, never clamped: past the end, empty, too big, or not whole
// blocks of this namespace (the block layer refuses that first; this is
// the driver not trusting it).
static int xfer_ok(int ns, uint32_t lba, uint32_t count, const void *buf) {
    if (g_dead || ns < 0 || ns >= g_ns_count || !buf || count == 0) return 0;
    const struct nvme_ns *n = &g_ns[ns];
    if (count * 512ull > g_max_bytes) return 0;
    if ((lba | count) & (n->spb - 1)) return 0;
    return lba <= n->sectors && count <= n->sectors - lba;
}

// The buffer can be DMA'd directly: PRP1 must be dword-aligned, and the
// whole buffer must be identity-mapped for its address to be physical.
static int direct(const void *buf, uint32_t bytes) {
    uint64_t a = (uint64_t)(uintptr_t)buf;
    return (a & 3) == 0 && a + bytes <= paging_identity_limit();
}

static void rw_cmd(struct nvme_sqe *c, int cid, int ns, const struct blk_io *io, uint64_t phys) {
    const struct nvme_ns *n = &g_ns[ns];
    uint64_t slba = io->lba / n->spb;
    k_memset(c, 0, sizeof *c);
    c->opc = io->write ? NVM_WRITE : NVM_READ;
    c->nsid = n->nsid;
    c->cdw10 = (uint32_t)slba;
    c->cdw11 = (uint32_t)(slba >> 32);
    c->cdw12 = (uint32_t)(io->count / n->spb) - 1;   // blocks, zero-based
    set_prps(c, cid, phys, (uint32_t)io->count * 512);
}

// One transfer through the bounce buffer.
static int bounced(int ns, struct blk_io *io) {
    uint32_t bytes = (uint32_t)io->count * 512;
    if (bytes > g_bounce_bytes) return 0;
    if (io->write) k_memcpy(g_bounce, io->buf, bytes);
    struct nvme_sqe c;
    rw_cmd(&c, 0, ns, io, (uint64_t)(uintptr_t)g_bounce);
    if (!submit_and_wait(&g_io, &c, 1) || g_io.status[0]) return 0;
    if (!io->write) k_memcpy(io->buf, g_bounce, bytes);
    return 1;
}

// Up to IO_INFLIGHT transfers, all direct, in one doorbell.
static void round_of(int ns, struct blk_io *io, int n) {
    static struct nvme_sqe cmds[IO_INFLIGHT];   // under g_lock
    for (int i = 0; i < n; i++) rw_cmd(&cmds[i], i, ns, &io[i], (uint64_t)(uintptr_t)io[i].buf);
    int ok = submit_and_wait(&g_io, cmds, n);
    for (int i = 0; i < n; i++) io[i].ok = (int8_t)(ok && g_io.status[i] == 0);
}

static int batch_locked(int ns, struct blk_io *io, int n) {
    int all = 1;
    for (int i = 0; i < n; ) {
        int k = 0;
        while (i + k < n && k < IO_INFLIGHT &&
               xfer_ok(ns, io[i + k].lba, io[i + k].count, io[i + k].buf) &&
               direct(io[i + k].buf, (uint32_t)io[i + k].count * 512))
            k++;
        if (k) {
            round_of(ns, &io[i], k);
        } else {
            k = 1;
            io[i].ok = (int8_t)(xfer_ok(ns, io[i].lba, io[i].count, io[i].buf) && bounced(ns, &io[i]));
        }
        for (int j = 0; j < k; j++) if (!io[i + j].ok) all = 0;
        i += k;
    }
    return all;
}

int nvme_submit_batch(int ns, struct blk_io *io, int n) {
    if (n <= 0) return 0;
    kmutex_lock(&g_lock);
    int r = batch_locked(ns, io, n);
    kmutex_unlock(&g_lock);
    return r;
}

int nvme_read(int ns, uint32_t lba, int count, void *buf) {
    if (count <= 0) return 0;
    struct blk_io io = { .lba = lba, .count = (uint16_t)count, .write = 0, .buf = buf };
    if ((uint32_t)count > 0xFFFF) return 0;
    return nvme_submit_batch(ns, &io, 1);
}

int nvme_write(int ns, uint32_t lba, int count, const void *buf) {
    if (count <= 0) return 0;
    struct blk_io io = { .lba = lba, .count = (uint16_t)count, .write = 1, .buf = (void *)buf };
    if ((uint32_t)count > 0xFFFF) return 0;
    return nvme_submit_batch(ns, &io, 1);
}

int nvme_flush(int ns) {
    if (g_dead || ns < 0 || ns >= g_ns_count) return 0;
    if (!g_vwc) return 1;          // nothing volatile to flush
    kmutex_lock(&g_lock);
    struct nvme_sqe c = { .opc = NVM_FLUSH, .nsid = g_ns[ns].nsid };
    int ok = submit_and_wait(&g_io, &c, 1) && g_io.status[0] == 0;
    kmutex_unlock(&g_lock);
    return ok;
}

// DATASET MANAGEMENT with the deallocate attribute: NVMe's TRIM. The
// range list is in the scratch page, 256 ranges of 16 bytes -- a full
// page, and the command's own limit.
int nvme_trim_ranges(int ns, const struct blk_range *r, int n) {
    if (!g_dsm || g_dead || ns < 0 || ns >= g_ns_count || n <= 0) return 0;
    const struct nvme_ns *s = &g_ns[ns];
    for (int i = 0; i < n; i++)
        if ((r[i].lba | r[i].count) & (s->spb - 1) || !r[i].count ||
            r[i].lba > s->sectors || r[i].count > s->sectors - r[i].lba) return 0;
    kmutex_lock(&g_lock);
    int ok = 1;
    for (int i = 0; i < n && ok; ) {
        int k = n - i < 256 ? n - i : 256;
        uint8_t *list = dma_at(OFF_SCRATCH);
        k_memset(list, 0, PAGE);
        for (int j = 0; j < k; j++) {
            uint8_t *e = list + j * 16;
            uint32_t nlb = r[i + j].count / s->spb;
            uint64_t slba = r[i + j].lba / s->spb;
            e[4] = (uint8_t)nlb; e[5] = (uint8_t)(nlb >> 8);
            e[6] = (uint8_t)(nlb >> 16); e[7] = (uint8_t)(nlb >> 24);
            for (int b = 0; b < 8; b++) e[8 + b] = (uint8_t)(slba >> (8 * b));
        }
        struct nvme_sqe c = { .opc = NVM_DSM, .nsid = s->nsid,
                              .prp1 = g_dma + OFF_SCRATCH,
                              .cdw10 = (uint32_t)k - 1, .cdw11 = 1u << 2 };
        ok = submit_and_wait(&g_io, &c, 1) && g_io.status[0] == 0;
        i += k;
    }
    kmutex_unlock(&g_lock);
    return ok;
}

int nvme_trim(int ns, uint32_t lba, uint32_t count) {
    struct blk_range one = { lba, count };
    return nvme_trim_ranges(ns, &one, 1);
}
