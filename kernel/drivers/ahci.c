// AHCI (SATA), one drive, DMA + interrupt. See kernel/include/kernel/ahci.h
// for what this drives and where it deliberately stops short of Linux's.
//
// THE ONE ORDERING THAT MATTERS, because getting it wrong hangs the
// machine rather than losing a transfer: the command engine is started
// only after PxCLB/PxFB point at real memory, and the interrupt line is
// unmasked only after the handler is registered. A level-triggered INTx
// asserted with nobody willing to clear PxIS is never deasserted.
#include "kmutex.h" // g_ahci_lock
#include "scheduler.h"   // a command wait SLEEPS -- see sleep_command()
#include "clocksource.h" // ...against a deadline
#include "paging.h"      // paging_identity_limit() -- zero-copy DMA
#include "ahci.h"
#include "block.h"   // blk_dsm_pack() -- the DSM payload, shared with ata.c
#include "pci.h"
#include "pci_internal.h"
#include "pmm.h"
#include "paging.h"
#include "irq.h"
#include "pic.h"
#include "timer.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "idt.h"
#include "barrier.h"
#include <stddef.h>
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`
#include "pci_driver.h"

DRIVER_DECLARE("ahci", "block", "SATA AHCI host controller");

// ---- the register map (AHCI 1.3.1, section 3) ------------------------

#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_VS    0x10
#define HBA_CAP2  0x24
#define HBA_BOHC  0x28

#define GHC_HR    (1u << 0)   // HBA reset
#define GHC_IE    (1u << 1)   // interrupts to the host
#define GHC_AE    (1u << 31)  // AHCI enable -- not IDE-emulation mode

#define CAP_NP    0x1Fu       // ports supported, minus one
#define CAP_NCS_SHIFT 8
#define CAP_NCS   0x1Fu       // command slots, minus one
#define CAP_SSS   (1u << 27)  // staggered spin-up
#define CAP_SNCQ  (1u << 30)
#define CAP_S64A  (1u << 31)

#define CAP2_BOH  (1u << 0)   // BIOS/OS handoff is implemented
#define BOHC_BOS  (1u << 0)   // BIOS owns the HBA
#define BOHC_OOS  (1u << 1)   // the OS is asking for it

#define PORT_BASE 0x100
#define PORT_SIZE 0x80

#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SCTL  0x2C
#define PX_SERR  0x30
#define PX_SACT  0x34   // NCQ tags outstanding -- set before PxCI, cleared by the drive
#define PX_CI    0x38

#define PXCMD_ST  (1u << 0)
#define PXCMD_SUD (1u << 1)   // spin-up device, only meaningful with CAP.SSS
#define PXCMD_FRE (1u << 4)   // FIS receive enable
#define PXCMD_FR  (1u << 14)  // FIS receive running
#define PXCMD_CR  (1u << 15)  // command list running

#define PXIS_TFES (1u << 30)  // task file error -- the drive refused

// The completion bits (D2H register FIS, PIO setup, DMA setup,
// set-device-bits) AND the fatal error bits. The errors have to be in
// here: without them a refused command raises no interrupt at all, and
// the waiter below sits out its whole one-second budget before
// reporting a TIMEOUT for something the drive answered immediately.
// Hot-plug and PHY events stay masked -- a handler with nothing to do
// about an event is worse than no handler.
#define PXIE_ERR  ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | (1u << 26))
#define PXIE_MASK (0x0000000Fu | PXIE_ERR)

#define PXTFD_ERR (1u << 0)
#define PXTFD_DRQ (1u << 3)
#define PXTFD_BSY (1u << 7)

#define SIG_SATA  0x00000101u

// ---- the command structures (AHCI 1.3.1, section 4.2) ---------------

struct cmd_header {
    uint16_t flags;      // CFL:5, A, W, P, R, B, C, rsvd, PMP:4
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t rsvd[4];
};

struct prd {
    uint32_t dba, dbau, rsvd;
    uint32_t dbc;        // bits 21:0 are the byte count MINUS ONE
};

#define PRDT_ENTRIES 64  // one per 4 KiB page of the bounce buffer -- 256 KiB

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsvd[48];
    struct prd prdt[PRDT_ENTRIES];
};

// ATA commands issued as a Register H2D FIS.
#define FIS_TYPE_H2D    0x27
#define ATA_IDENTIFY    0xEC
#define ATA_READ_DMA_EX 0x25
#define ATA_WRITE_DMA_E 0x35
#define ATA_FLUSH_EXT   0xEA
#define ATA_DSM         0x06
#define ATA_READ_FPDMA  0x60   // READ FPDMA QUEUED -- NCQ
#define ATA_WRITE_FPDMA 0x61
#define DSM_FEATURE_TRIM 0x01

// DSM's payload layout is blk_dsm_pack()'s (block.h), shared with ata.c.

// ---- state ----------------------------------------------------------

#define AHCI_MAX_PORTS 32
#define DMA_BUF_FRAMES PRDT_ENTRIES   // 256 KiB = 512 sectors per command

static volatile uint8_t *g_abar;
static const struct pci_device *g_pci;
static uint32_t g_cap, g_pi;
static int g_port_count;
static struct ahci_port_status g_ports[AHCI_MAX_PORTS];

static int g_active = -1;             // index into g_ports, not a port number
static volatile uint8_t *g_preg;      // the active port's register block
static struct cmd_header *g_clist;
static struct cmd_table *g_ctable;
static uint8_t *g_buf;
static uint64_t g_clist_phys, g_fis_phys, g_ctable_phys, g_buf_phys;
static uint32_t g_buf_frames;

static uint32_t g_sectors;
static int g_lba48;
static int g_trim;                    // IDENTIFY word 169 bit 0
static char g_model[41];
static uint8_t g_irq;          // INTx line, 0 when not on one
static uint8_t g_msi_vector;   // LAPIC vector, 0 when not on one
static volatile int g_irq_fired;
static volatile uint32_t g_irq_status;   // PxIS as the handler saw it

// What a command waiter parks on and the handler wakes -- its ADDRESS
// is the channel. And how many waits parked rather than polled, the
// evidence the sleep path is the one in use (QUERY_AHCI).
static const char g_cmd_chan;
static uint64_t g_cmd_sleeps;

// ---- NCQ state ------------------------------------------------------
//
// ONE TABLE PER TAG, because a queued command's table must stay put
// until the drive has fetched it and there is no knowing when that is.
// Small on purpose: a queued command is at most NCQ_MAX_SECTORS and
// DMAs straight into the caller's buffer (ncq_addressable()), so it
// needs a page-split PRDT, not the bounce buffer's 64 entries.
#define NCQ_MAX         32
#define NCQ_MAX_SECTORS 128               // 64 KiB per queued command
#define NCQ_PRDT        17                // 64 KiB page-split, +1 misaligned
#define NCQ_WAIT_TICKS  500               // 5 s for a whole round
struct ncq_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsvd[48];
    struct prd prdt[NCQ_PRDT];
} __attribute__((aligned(128)));          // the HBA's CTBA alignment
static struct ncq_table *g_ncq;
static uint64_t g_ncq_phys;
static int g_ncq_depth;                   // tags in use; 0 = no NCQ
static int g_ncq_drive_depth;             // IDENTIFY word 75 + 1, 0 = none
static uint64_t g_ncq_rounds, g_ncq_cmds, g_ncq_fallbacks;

// ~1 s at PIT_HZ, the same budget ata.c gives a transfer. A drive that
// has not answered in a second under an emulator is not going to.
#define WAIT_TICKS 100
// The interrupts-off bound, where pit_ticks() cannot advance -- same
// reasoning as ata.c's ATA_POLL_LIMIT.
#define POLL_LIMIT 2000000

static inline uint32_t hba_r(uint32_t off) { return *(volatile uint32_t *)(g_abar + off); }
static inline void hba_w(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_abar + off) = v; }
static inline uint32_t px_r(volatile uint8_t *p, uint32_t off) { return *(volatile uint32_t *)(p + off); }
static inline void px_w(volatile uint8_t *p, uint32_t off, uint32_t v) { *(volatile uint32_t *)(p + off) = v; }

static volatile uint8_t *port_regs(int port) {
    return g_abar + PORT_BASE + (uint32_t)port * PORT_SIZE;
}

// Waits for `mask` to read back clear in `reg`, up to ~`ticks`. Returns
// 0 on timeout. Bounded by an iteration count as well, because
// pit_ticks() is frozen when this is reached with interrupts off.
static int wait_clear(volatile uint8_t *p, uint32_t reg, uint32_t mask, uint64_t ticks) {
    uint64_t start = pit_ticks();
    for (uint64_t guard = 0; guard < (uint64_t)POLL_LIMIT; guard++) {
        if (!(px_r(p, reg) & mask)) return 1;
        if (pit_ticks() - start > ticks) return 0;
    }
    return 0;
}

// ---- port bring-up ---------------------------------------------------

static void port_stop(volatile uint8_t *p) {
    uint32_t cmd = px_r(p, PX_CMD);
    px_w(p, PX_CMD, cmd & ~PXCMD_ST);
    wait_clear(p, PX_CMD, PXCMD_CR, WAIT_TICKS);
    cmd = px_r(p, PX_CMD);
    px_w(p, PX_CMD, cmd & ~PXCMD_FRE);
    wait_clear(p, PX_CMD, PXCMD_FR, WAIT_TICKS);
}

// Starts the engine. Refuses while the drive is busy: PxCMD.ST set with
// BSY or DRQ still asserted is the one sequence the spec calls out as
// undefined, and it presents as a port that accepts commands and never
// completes one.
static int port_start(volatile uint8_t *p) {
    if (!wait_clear(p, PX_TFD, PXTFD_BSY | PXTFD_DRQ, WAIT_TICKS)) return 0;
    px_w(p, PX_CMD, px_r(p, PX_CMD) | PXCMD_FRE);
    px_w(p, PX_CMD, px_r(p, PX_CMD) | PXCMD_ST);
    return 1;
}

// A FATAL ERROR STOPS THE COMMAND ENGINE, and nothing restarts it on its
// own -- so without this the first refused command wedges the drive and
// every later transfer times out, which reads as a dead disk rather than
// as one bad sector. AHCI 1.3.1's 6.2.2 recovery: stop, clear PxSERR,
// clear PxIS, start.
//
// UNEXERCISED BY ANY TEST HERE, and deliberately said out loud: the
// bounds check refuses a bad LBA before the drive ever sees one, and
// QEMU produces no media errors. See docs/roadmap.md.
static void port_recover(volatile uint8_t *p) {
    port_stop(p);
    px_w(p, PX_SERR, px_r(p, PX_SERR));
    px_w(p, PX_IS, px_r(p, PX_IS));
    if (!port_start(p)) klog_write(KLOG_ERR "ahci: the port would not restart after an error\n");
}

// ---- issuing one command --------------------------------------------

// Fills slot 0's Register H2D FIS. `count` is in sectors; `lba` is
// 48-bit even on a 28-bit drive, because READ/WRITE DMA EXT is the only
// command this driver issues and it has no 28-bit form.
static void build_fis(uint8_t command, uint8_t features, uint64_t lba, uint16_t count) {
    uint8_t *f = g_ctable->cfis;
    k_memset(f, 0, 64);
    f[0] = FIS_TYPE_H2D;
    f[1] = 0x80;                 // C: this is a command, not a control update
    f[2] = command;
    f[3] = features;             // DSM's TRIM bit; zero for everything else
    f[4] = (uint8_t)(lba);
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                 // LBA mode; no drive/head select on SATA
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)(count);
    f[13] = (uint8_t)(count >> 8);
}

// One PRD per 4 KiB page, which is the shape Linux's ahci_fill_sg()
// produces and what keeps the multi-entry path ordinary rather than
// dead. `bytes` of 0 means a command that moves no data (FLUSH), which
// takes no PRD at all -- a zero-length entry is not the same thing and
// some drives reject it.
static int build_prdt(uint32_t bytes) {
    int n = 0;
    uint32_t done = 0;
    while (done < bytes && n < PRDT_ENTRIES) {
        uint32_t chunk = bytes - done;
        if (chunk > 4096) chunk = 4096;
        g_ctable->prdt[n].dba  = (uint32_t)(g_buf_phys + done);
        g_ctable->prdt[n].dbau = 0;
        g_ctable->prdt[n].rsvd = 0;
        g_ctable->prdt[n].dbc  = chunk - 1;   // byte count MINUS ONE
        done += chunk;
        n++;
    }
    return (done == bytes) ? n : -1;
}

static void irq_handler(uint64_t *regs) {
    (void)regs;
    if (!g_abar || !g_preg) return;
    uint32_t is = hba_r(HBA_IS);
    if (!is) return;                    // a shared line, and not ours
    uint32_t pis = px_r(g_preg, PX_IS);
    // PORT FIRST, THEN THE HBA. Clearing HBA_IS while PxIS still holds
    // a bit leaves the port re-asserting immediately, which on a
    // level-triggered line is a hang rather than a lost completion.
    px_w(g_preg, PX_IS, pis);
    hba_w(HBA_IS, is);
    // ACCUMULATED, not overwritten: with several NCQ tags in flight one
    // interrupt can carry a completion and the next an error, and the
    // waiter reads the union. run_command()/a batch round zero it first.
    g_irq_status |= pis;
    g_irq_fired = 1;
    scheduler_wake(&g_cmd_chan, 0);   // interrupt-safe, by contract
}

// Has slot 0 retired, or failed? The handler having run is one answer;
// PxCI clearing (or a task-file error, which retires the command WITHOUT
// clearing it) is the other, and is true earlier when interrupts are off.
// Found by reading the port, the ack the handler would have done is done
// here -- unless the handler already ran, whose PxIS copy is the real one.
static int command_done(void) {
    if (g_irq_fired) return 1;
    uint32_t is = px_r(g_preg, PX_IS);
    if ((px_r(g_preg, PX_CI) & 1u) && !(is & PXIS_TFES)) return 0;
    g_irq_status |= is;
    px_w(g_preg, PX_IS, is);
    hba_w(HBA_IS, hba_r(HBA_IS));
    return 1;
}

// Parks the caller until the command retires or `ticks` pass -- the only
// wait that lets anything else run, and ata.c's sleep_dma_irq() shape.
// 1 done, 0 timed out, -1 COULD NOT PARK (no scheduler slot, the
// preemption guard up, or no interrupt at all), and the caller polls.
static int sleep_command(uint64_t ticks) {
    if (!ahci_irq_driven()) return -1;
    uint64_t deadline = clocksource_now_ns() + ticks * (1000000000ull / PIT_HZ);
    for (;;) {
        scheduler_wait_arm(&g_cmd_chan);
        if (command_done()) { scheduler_wait_disarm(); return 1; }
        if (clocksource_now_ns() >= deadline) { scheduler_wait_disarm(); return 0; }
        if (!scheduler_block_kernel_until(&g_cmd_chan, SCHED_WAIT_DISK, deadline)) {
            scheduler_wait_disarm();
            return -1;
        }
        g_cmd_sleeps++;
    }
}

// Waits for slot 0 to retire. A scheduled caller SLEEPS until the
// handler wakes it (sleep_command()). One with nowhere to park polls
// PxCI when interrupts are off (a legacy `run`, a guarded caller) and
// halts until the interrupt otherwise; the hardware clears PxCI either
// way, so every path observes the same completion.
static int wait_command(void) {
    int slept = sleep_command(WAIT_TICKS);
    if (slept == 0) return 0;
    if (slept == 1) {
        if (g_irq_status & PXIS_TFES) return 1;
        return wait_clear(g_preg, PX_CI, 1u, WAIT_TICKS);
    }

    // NOWHERE TO PARK. The hlt wait needs an interrupt -- INTx OR MSI;
    // this tested `g_irq` alone, so an MSI controller (the ASUS's) always
    // polled, even from the kernel context.
    if (isr_in_progress() || !ahci_irq_driven()) {
        for (uint64_t i = 0; i < (uint64_t)POLL_LIMIT; i++) {
            if (!(px_r(g_preg, PX_CI) & 1u)) {
                g_irq_status = px_r(g_preg, PX_IS);
                px_w(g_preg, PX_IS, g_irq_status);
                hba_w(HBA_IS, hba_r(HBA_IS));
                return 1;
            }
            // A task file error retires the command without clearing
            // PxCI, so this is a completion to report, not a timeout.
            if (px_r(g_preg, PX_IS) & PXIS_TFES) {
                g_irq_status = px_r(g_preg, PX_IS);
                px_w(g_preg, PX_IS, g_irq_status);
                hba_w(HBA_IS, hba_r(HBA_IS));
                return 1;
            }
        }
        return 0;
    }

    uint64_t start = pit_ticks();
    while (!g_irq_fired) {
        if (pit_ticks() - start > WAIT_TICKS) return 0;
        __asm__ volatile ("hlt");
    }
    // The completion interrupt and the slot retiring are two events, and
    // an error interrupt arrives with PxCI still set.
    if (g_irq_status & PXIS_TFES) return 1;
    return wait_clear(g_preg, PX_CI, 1u, WAIT_TICKS);
}

// Issues slot 0 and waits. `bytes` is how much data the PRDT should
// describe; `write` says which way it moves. Returns 1 on success.
static int run_command(uint8_t command, uint8_t features, uint64_t lba, uint16_t count,
                       uint32_t bytes, int write) {
    if (!g_preg) return 0;

    int prdtl = build_prdt(bytes);
    if (prdtl < 0) return 0;

    build_fis(command, features, lba, count);

    // CFL is the FIS length in DWORDS -- a Register H2D FIS is 20 bytes,
    // five dwords, NOT the 64-byte slot it sits in. Bit 6 is W.
    g_clist[0].flags = (uint16_t)(5u | (write ? (1u << 6) : 0));
    g_clist[0].prdtl = (uint16_t)prdtl;
    g_clist[0].prdbc = 0;
    g_clist[0].ctba  = (uint32_t)g_ctable_phys;
    g_clist[0].ctbau = 0;

    if (!wait_clear(g_preg, PX_TFD, PXTFD_BSY | PXTFD_DRQ, WAIT_TICKS)) return 0;

    px_w(g_preg, PX_IS, px_r(g_preg, PX_IS));   // stale bits from a previous error
    px_w(g_preg, PX_SERR, px_r(g_preg, PX_SERR));
    g_irq_fired = 0;
    g_irq_status = 0;
    kmb();                                      // the tables above must land first
    px_w(g_preg, PX_CI, 1u);

    if (!wait_command()) {
        klog_printf(KLOG_ERR "ahci: command 0x%x timed out (tfd 0x%x, is 0x%x)\n",
                    command, px_r(g_preg, PX_TFD), px_r(g_preg, PX_IS));
        return 0;
    }
    kmb();

    if (g_irq_status & PXIS_TFES) {
        klog_printf(KLOG_ERR "ahci: command 0x%x refused (tfd 0x%x)\n", command, px_r(g_preg, PX_TFD));
        port_recover(g_preg);
        return 0;
    }
    if (px_r(g_preg, PX_TFD) & PXTFD_ERR) {
        klog_printf("ahci: command 0x%x set ERR (tfd 0x%x)\n", command, px_r(g_preg, PX_TFD));
        return 0;
    }
    return 1;
}

// ---- NCQ: several commands in flight ------------------------------
//
// READ/WRITE FPDMA QUEUED, one tag per command: set the tag's PxSACT bit,
// then its PxCI bit, and the drive clears PxSACT bits as tags complete
// (reported by a Set Device Bits FIS, which PXIE_MASK already enables).
// The latency of each command OVERLAPS instead of adding -- which is the
// only lever on a device whose cost is per command (measured: ~75 us of
// the ~100 us an emulated AHCI read takes is waiting for the device).
// Linux's libata issues the same commands; the error handling here is
// deliberately simpler: see ncq_round().

// Turns NCQ on when both ends offer it and completions interrupt; a
// polled controller gains nothing from a queue it has to spin on.
static void ncq_setup(void) {
    if (!(g_cap & CAP_SNCQ) || !g_ncq_drive_depth || !ahci_irq_driven()) return;
    int depth = g_ncq_drive_depth;
    if (depth > ahci_command_slots()) depth = ahci_command_slots();
    if (depth > NCQ_MAX) depth = NCQ_MAX;
    uint32_t bytes = (uint32_t)depth * (uint32_t)sizeof(struct ncq_table);
    uint64_t base = pmm_alloc_contiguous((bytes + 4095) / 4096, PMM_ZONE_DMA32);
    if (!base) return;                    // no NCQ, and nothing else lost
    k_memset((void *)(uintptr_t)base, 0, bytes);
    g_ncq = (struct ncq_table *)(uintptr_t)base;
    g_ncq_phys = base;
    g_ncq_depth = depth;
}

int ahci_ncq_depth(void) { return g_ncq_depth; }

// May the drive DMA straight into `buf`? Identity-mapped (so its virtual
// address IS its physical one -- virtio-blk's rule), word-aligned (a PRD
// base's bit 0 is reserved), and below 4 GiB unless the HBA has 64-bit
// addressing. Anything else takes the bounce buffer, one at a time.
static int ncq_addressable(const struct blk_io *io) {
    uint64_t a = (uint64_t)(uintptr_t)io->buf;
    uint64_t end = a + (uint64_t)io->count * AHCI_SECTOR_SIZE;
    if (!io->buf || !io->count || io->count > NCQ_MAX_SECTORS) return 0;
    if ((a & 1) || end > paging_identity_limit()) return 0;
    if (!(g_cap & CAP_S64A) && end > 0x100000000ull) return 0;
    return 1;
}

static void ncq_build(int tag, const struct blk_io *io) {
    struct ncq_table *t = &g_ncq[tag];
    uint64_t a = (uint64_t)(uintptr_t)io->buf;
    uint32_t bytes = (uint32_t)io->count * AHCI_SECTOR_SIZE, done = 0;
    int n = 0;
    while (done < bytes) {                // page-split: n stays <= NCQ_PRDT
        uint32_t chunk = 4096 - (uint32_t)((a + done) & 4095);
        if (chunk > bytes - done) chunk = bytes - done;
        t->prdt[n].dba  = (uint32_t)(a + done);
        t->prdt[n].dbau = (uint32_t)((a + done) >> 32);
        t->prdt[n].rsvd = 0;
        t->prdt[n].dbc  = chunk - 1;
        done += chunk;
        n++;
    }
    uint8_t *f = t->cfis;
    uint64_t lba = io->lba;
    k_memset(f, 0, 64);
    f[0]  = FIS_TYPE_H2D;
    f[1]  = 0x80;
    f[2]  = io->write ? ATA_WRITE_FPDMA : ATA_READ_FPDMA;
    f[3]  = (uint8_t)io->count;           // FPDMA: the count is in FEATURES
    f[4]  = (uint8_t)lba;
    f[5]  = (uint8_t)(lba >> 8);
    f[6]  = (uint8_t)(lba >> 16);
    f[7]  = 0x40;
    f[8]  = (uint8_t)(lba >> 24);
    f[9]  = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[11] = (uint8_t)(io->count >> 8);
    f[12] = (uint8_t)(tag << 3);          // ...and the TAG is in the count
    uint64_t phys = g_ncq_phys + (uint64_t)tag * sizeof(struct ncq_table);
    g_clist[tag].flags = (uint16_t)(5u | (io->write ? (1u << 6) : 0));
    g_clist[tag].prdtl = (uint16_t)n;
    g_clist[tag].prdbc = 0;
    g_clist[tag].ctba  = (uint32_t)phys;
    g_clist[tag].ctbau = (uint32_t)(phys >> 32);
}

// 1 every tag in `mask` completed, -1 the drive reported an error, 0 not
// yet. The accumulated g_irq_status carries an error the handler already
// cleared out of PxIS.
static int ncq_state(uint32_t mask) {
    if ((px_r(g_preg, PX_IS) | g_irq_status) & PXIS_TFES) return -1;
    if ((px_r(g_preg, PX_SACT) | px_r(g_preg, PX_CI)) & mask) return 0;
    return 1;
}

// Waits for ncq_state() to leave 0: asleep when the caller can park,
// halted on the interrupt from the kernel context, polling with
// interrupts off. Returns the final state, 0 on timeout.
static int ncq_wait(uint32_t mask) {
    uint64_t deadline = clocksource_now_ns() + (uint64_t)NCQ_WAIT_TICKS * (1000000000ull / PIT_HZ);
    for (;;) {
        scheduler_wait_arm(&g_cmd_chan);
        int st = ncq_state(mask);
        if (st) { scheduler_wait_disarm(); return st; }
        if (clocksource_now_ns() >= deadline) { scheduler_wait_disarm(); return 0; }
        if (!scheduler_block_kernel_until(&g_cmd_chan, SCHED_WAIT_DISK, deadline)) {
            scheduler_wait_disarm();
            break;                        // nowhere to park
        }
        g_cmd_sleeps++;
    }
    if (!isr_in_progress()) {
        uint64_t start = pit_ticks();
        for (;;) {
            int st = ncq_state(mask);
            if (st) return st;
            if (pit_ticks() - start > NCQ_WAIT_TICKS) return 0;
            __asm__ volatile ("hlt");
        }
    }
    for (uint64_t i = 0; i < (uint64_t)POLL_LIMIT * 4; i++) {
        int st = ncq_state(mask);
        if (st) return st;
    }
    return 0;
}

// One round: io[0..n) (n <= g_ncq_depth, every one ncq_addressable())
// in flight together. Returns 1 if the drive completed all of them.
//
// **AN ERROR ABANDONS THE WHOLE ROUND**, and the caller replays it one
// command at a time: a failed queued command aborts every outstanding
// tag, and which one failed is in the drive's NCQ error log (READ LOG
// EXT page 10h) -- what Linux reads. Replaying through the ordinary path
// gives each transfer its own answer without that machinery, at the cost
// of repeating the round's good transfers, which is only ever correct
// for a transfer that has not been acknowledged to anyone yet.
static int ncq_round(struct blk_io *io, int n) {
    uint32_t mask = 0;
    for (int t = 0; t < n; t++) { ncq_build(t, &io[t]); mask |= 1u << t; }
    if (!wait_clear(g_preg, PX_TFD, PXTFD_BSY | PXTFD_DRQ, WAIT_TICKS)) return 0;
    px_w(g_preg, PX_IS, px_r(g_preg, PX_IS));
    px_w(g_preg, PX_SERR, px_r(g_preg, PX_SERR));
    g_irq_fired = 0;
    g_irq_status = 0;
    kmb();                                // tables before the doorbell
    px_w(g_preg, PX_SACT, mask);          // SACT first -- AHCI 1.3.1 5.3.2.1
    px_w(g_preg, PX_CI, mask);
    int st = ncq_wait(mask);
    kmb();
    if (st == 1) {
        g_ncq_rounds++;
        g_ncq_cmds += (uint64_t)n;
        return 1;
    }
    klog_printf(KLOG_ERR "ahci: NCQ round of %d %s (sact 0x%x, ci 0x%x, tfd 0x%x) -- "
                "replaying it one command at a time\n", n, st < 0 ? "failed" : "timed out",
                px_r(g_preg, PX_SACT), px_r(g_preg, PX_CI), px_r(g_preg, PX_TFD));
    port_recover(g_preg);                 // clearing ST drops every tag
    g_ncq_fallbacks++;
    return 0;
}

// ---- IDENTIFY -------------------------------------------------------

// IDENTIFY's model string is 20 big-endian 16-bit words, i.e. every
// pair of bytes is swapped, and it is space-padded rather than
// terminated.
static void copy_model(const uint16_t *id) {
    for (int i = 0; i < 20; i++) {
        g_model[i * 2]     = (char)(id[27 + i] >> 8);
        g_model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    g_model[40] = 0;
    for (int i = 39; i >= 0 && g_model[i] == ' '; i--) g_model[i] = 0;
}

static int identify(void) {
    if (!run_command(ATA_IDENTIFY, 0, 0, 0, 512, 0)) return 0;

    const uint16_t *id = (const uint16_t *)g_buf;
    copy_model(id);

    // Word 169 bit 0: DATA SET MANAGEMENT's TRIM bit is supported.
    g_trim = (id[169] & 0x0001) != 0;
    // Word 76 bit 8: NCQ; word 75 bits 4:0: its queue depth minus one.
    g_ncq_drive_depth = (id[76] & (1u << 8)) ? (id[75] & 0x1F) + 1 : 0;

    uint64_t sectors;
    if (id[83] & (1u << 10)) {          // 48-bit addressing supported
        g_lba48 = 1;
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                  ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    } else {
        g_lba48 = 0;
        sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    }
    if (!sectors) return 0;

    // The block layer counts sectors in 32 bits. Clamping and SAYING SO
    // beats wrapping: a silently truncated capacity is a filesystem
    // that formats fine and corrupts past 2 TiB.
    if (sectors > 0xFFFFFFFFull) {
        klog_printf("ahci: drive reports %llu sectors -- clamped to 2 TiB (32-bit block layer)\n",
                    (unsigned long long)sectors);
        sectors = 0xFFFFFFFFull;
    }
    g_sectors = (uint32_t)sectors;
    return 1;
}

// ---- init ------------------------------------------------------------

// Some firmware owns the HBA until asked for it. QEMU does not
// implement the handoff at all, which is why this is guarded on CAP2
// rather than run blind.
static void bios_handoff(void) {
    if (!(hba_r(HBA_CAP2) & CAP2_BOH)) return;
    hba_w(HBA_BOHC, hba_r(HBA_BOHC) | BOHC_OOS);
    uint64_t start = pit_ticks();
    while (hba_r(HBA_BOHC) & BOHC_BOS) {
        if (pit_ticks() - start > WAIT_TICKS) {
            klog_write("ahci: firmware did not release the HBA -- taking it anyway\n");
            return;
        }
    }
}

// prog_if 0x01 is "AHCI 1.0"; a 0x06 subclass with anything else there
// is a SATA controller in some vendor-specific mode this driver cannot
// speak, so the match names it.
static const struct pci_match ahci_matches[] = { PCI_MATCH_CLASS(0x01, 0x06, 0x01) };

// Records every implemented port and what is on it. Enumeration is
// separate from bring-up because the answer is worth reporting even for
// the ports this driver will not use.
static void scan_ports(void) {
    for (int port = 0; port < AHCI_MAX_PORTS; port++) {
        if (!(g_pi & (1u << port))) continue;
        volatile uint8_t *p = port_regs(port);
        uint32_t ssts = px_r(p, PX_SSTS);
        uint32_t cmd  = px_r(p, PX_CMD);
        struct ahci_port_status *s = &g_ports[g_port_count++];
        s->port      = (uint8_t)port;
        s->det       = (uint8_t)(ssts & 0xF);
        s->ipm       = (uint8_t)((ssts >> 8) & 0xF);
        s->speed     = (uint8_t)((ssts >> 4) & 0xF);
        s->signature = px_r(p, PX_SIG);
        s->running   = (cmd & (PXCMD_ST | PXCMD_FRE)) == (PXCMD_ST | PXCMD_FRE);
        s->active    = 0;
    }
}

// The command list, the FIS receive area and the one command table all
// live in the first frame; the bounce buffer follows. The alignments
// the spec demands (1 KiB, 256 B, 128 B) are satisfied by the offsets
// below given a page-aligned base, which is what pmm_alloc_contiguous()
// returns -- so there is no alignment arithmetic to get wrong.
#define OFF_CLIST  0x000   // 32 headers, 1 KiB
#define OFF_FIS    0x400   // 256 B
#define OFF_CTABLE 0x500   // 128 + 16 * PRDT_ENTRIES = 1152 B, inside the frame

static int alloc_dma(void) {
    // STEPPED DOWN, NOT A CLIFF. 64 contiguous frames is a big ask of a
    // fragmented pool, and dropping straight to one would cost 64x the
    // commands for a shortfall that 16 would have absorbed.
    static const uint32_t TRY[] = { DMA_BUF_FRAMES, 16, 1 };
    uint32_t frames = 0;
    uint64_t base = 0;
    for (unsigned i = 0; i < sizeof TRY / sizeof TRY[0]; i++) {
        frames = TRY[i];
        base = pmm_alloc_contiguous(1 + frames, PMM_ZONE_DMA32);
        if (base) break;
    }
    if (!base) return 0;
    if (frames != DMA_BUF_FRAMES)
        klog_printf("ahci: only got a %u KiB DMA buffer (contiguous pool too fragmented)\n",
                    frames * 4);
    g_buf_frames = frames;
    g_clist_phys  = base + OFF_CLIST;
    g_fis_phys    = base + OFF_FIS;
    g_ctable_phys = base + OFF_CTABLE;
    g_buf_phys    = base + 4096;
    // Identity-mapped below 4 GiB, the same assumption ata.c and
    // virtio's register windows make.
    g_clist  = (struct cmd_header *)(uintptr_t)g_clist_phys;
    g_ctable = (struct cmd_table *)(uintptr_t)g_ctable_phys;
    g_buf    = (uint8_t *)(uintptr_t)g_buf_phys;
    k_memset((void *)(uintptr_t)base, 0, 4096);
    return 1;
}

// Brings up `index`'s port and IDENTIFYs whatever is on it. Leaves the
// port stopped again on failure, so a second candidate can be tried
// without inheriting half-configured state.
static int claim_port(int index) {
    struct ahci_port_status *s = &g_ports[index];
    volatile uint8_t *p = port_regs(s->port);

    port_stop(p);
    px_w(p, PX_CLB,  (uint32_t)g_clist_phys);
    px_w(p, PX_CLBU, 0);
    px_w(p, PX_FB,   (uint32_t)g_fis_phys);
    px_w(p, PX_FBU,  0);
    px_w(p, PX_SERR, px_r(p, PX_SERR));
    px_w(p, PX_IS,   px_r(p, PX_IS));
    if (g_cap & CAP_SSS) px_w(p, PX_CMD, px_r(p, PX_CMD) | PXCMD_SUD);

    if (!port_start(p)) {
        klog_printf("ahci: port %u would not start (tfd 0x%x)\n", s->port, px_r(p, PX_TFD));
        return 0;
    }

    g_preg = p;
    if (!identify()) {
        port_stop(p);
        g_preg = 0;
        return 0;
    }
    s->running = 1;
    s->active = 1;
    g_active = index;
    return 1;
}

static void ahci_probe(const struct pci_device *dev) {
    if (g_pci) {
        klog_printf("ahci: a second HBA at %02x:%02x.%u -- one is driven\n",
                    dev->bus, dev->device, dev->function);
        return;
    }
    g_pci = dev;

    uint64_t abar = pci_bar_mem_addr(g_pci, 5);
    if (!abar) {
        klog_write("ahci: controller found but BAR5 is unimplemented or I/O space\n");
        return;
    }
    g_abar = (volatile uint8_t *)paging_map_device(abar, 0x1100);
    if (!g_abar) {
        klog_printf(KLOG_ERR "ahci: ABAR at 0x%llx could not be mapped\n", (unsigned long long)abar);
        return;
    }

    pci_command_update(g_pci, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE, 0);

    bios_handoff();
    hba_w(HBA_GHC, hba_r(HBA_GHC) | GHC_AE);   // before ANY other register means what it says
    g_cap = hba_r(HBA_CAP);
    g_pi  = hba_r(HBA_PI);
    scan_ports();

    uint32_t vs = hba_r(HBA_VS);
    klog_printf("ahci: HBA %u.%u%u, %d port%s implemented, %d command slot%s\n",
                (vs >> 16) & 0xFFFF, (vs >> 8) & 0xFF, vs & 0xFF,
                g_port_count, g_port_count == 1 ? "" : "s",
                ahci_command_slots(), ahci_command_slots() == 1 ? "" : "s");

    if (!g_port_count) return;
    if (!alloc_dma()) {
        klog_write("ahci: out of contiguous memory for the command list -- no drive claimed\n");
        return;
    }

    for (int i = 0; i < g_port_count; i++) {
        // DET 3 is "device present, PHY communication established"; the
        // signature separates a disk from an ATAPI drive, an enclosure
        // or a port multiplier, none of which this driver speaks.
        if (g_ports[i].det != 3 || g_ports[i].signature != SIG_SATA) continue;
        if (claim_port(i)) break;
    }

    if (g_active < 0) {
        klog_write("ahci: no SATA drive on any implemented port\n");
        return;
    }

    // THE COMMIT POINT. Everything the handler reads exists now; before
    // this the device is free to assert nothing.
    // A vector if the controller offers one, else the pin. QEMU's
    // ich9-ahci advertises neither capability, so this takes the pin
    // on every emulated boot; a real ICH9-and-later part has MSI.
    uint8_t line = pci_irq_line(g_pci);
    g_msi_vector = pci_msi_request(g_pci, irq_handler);
    if (g_msi_vector) {
        px_w(g_preg, PX_IE, PXIE_MASK);
        hba_w(HBA_GHC, hba_r(HBA_GHC) | GHC_IE);
    } else if (line != IRQ_NONE) {
        g_irq = line;
        irq_register_handler(g_irq, irq_handler);
        px_w(g_preg, PX_IE, PXIE_MASK);
        hba_w(HBA_GHC, hba_r(HBA_GHC) | GHC_IE);
        pci_command_update(g_pci, 0, PCI_CMD_INTX_DISABLE);
        irq_unmask(g_irq);
    }

    ncq_setup();

    klog_printf("ahci: port %u: \"%s\", %u sectors, LBA%s, %d sectors/transfer, %s\n",
                g_ports[g_active].port, g_model, g_sectors, g_lba48 ? "48" : "28",
                ahci_max_sectors_per_xfer(),
                (g_irq || g_msi_vector) ? "IRQ-driven" : "polled (no interrupt line)");
    if (g_msi_vector)
        klog_printf("ahci: on %s vector %u\n",
                    g_pci->irq_msix ? "MSI-X" : "MSI", g_msi_vector);
    else if (g_irq) klog_printf("ahci: on IRQ %u\n", g_irq);
}

// ---- transfers -------------------------------------------------------

int ahci_present(void) { return g_active >= 0 && g_sectors > 0; }
uint32_t ahci_sector_count(void) { return ahci_present() ? g_sectors : 0; }
int ahci_max_sectors_per_xfer(void) { return (int)(g_buf_frames * 4096 / AHCI_SECTOR_SIZE); }

// A transfer is refused rather than clamped when it runs past the end
// of the drive: a short read that reports success is how a filesystem
// ends up parsing whatever the bounce buffer held last.
// ONE COMMAND AT A TIME: one command slot and one bounce buffer
// (g_buf), shared by every caller. Every public I/O entry takes this; it
// was the filesystem's lock alone, which a raw block user does not hold.
static struct kmutex g_ahci_lock;

static int bounds_ok(uint32_t lba, int count) {
    if (count <= 0 || count > ahci_max_sectors_per_xfer()) return 0;
    if (lba > g_sectors || (uint32_t)count > g_sectors - lba) return 0;
    return 1;
}

static int read_sectors(uint32_t lba, int count, void *buf) {
    if (!ahci_present() || !buf || !bounds_ok(lba, count)) return 0;
    uint32_t bytes = (uint32_t)count * AHCI_SECTOR_SIZE;
    if (!run_command(ATA_READ_DMA_EX, 0, lba, (uint16_t)count, bytes, 0)) return 0;
    k_memcpy(buf, g_buf, bytes);
    return 1;
}

int ahci_read_sectors(uint32_t lba, int count, void *buf) {
    kmutex_lock(&g_ahci_lock);
    int r = read_sectors(lba, count, buf);
    kmutex_unlock(&g_ahci_lock);
    return r;
}

static int write_sectors(uint32_t lba, int count, const void *buf) {
    if (!ahci_present() || !buf || !bounds_ok(lba, count)) return 0;
    uint32_t bytes = (uint32_t)count * AHCI_SECTOR_SIZE;
    k_memcpy(g_buf, buf, bytes);
    return run_command(ATA_WRITE_DMA_E, 0, lba, (uint16_t)count, bytes, 1);
}

int ahci_write_sectors(uint32_t lba, int count, const void *buf) {
    kmutex_lock(&g_ahci_lock);
    int r = write_sectors(lba, count, buf);
    kmutex_unlock(&g_ahci_lock);
    return r;
}

// The block layer's submit_batch: queue as many as the drive takes at
// once, in ROUNDS of up to g_ncq_depth. A transfer that cannot be queued
// (a buffer outside the identity map, one larger than NCQ_MAX_SECTORS)
// goes through the ordinary one-at-a-time path in its place, and so does
// every transfer of a round that failed -- see ncq_round().
int ahci_submit_batch(struct blk_io *io, int n) {
    if (n <= 0) return 0;
    kmutex_lock(&g_ahci_lock);
    int all = 1;
    for (int i = 0; i < n; ) {
        int k = 0;
        if (g_ncq_depth && ahci_present())
            while (i + k < n && k < g_ncq_depth && ncq_addressable(&io[i + k]) &&
                   bounds_ok(io[i + k].lba, io[i + k].count))
                k++;
        if (k >= 2 && ncq_round(&io[i], k)) {
            for (int j = 0; j < k; j++) io[i + j].ok = 1;
        } else {
            if (k == 0) k = 1;            // this one cannot be queued
            for (int j = 0; j < k; j++) {
                struct blk_io *x = &io[i + j];
                x->ok = (int8_t)(x->write ? write_sectors(x->lba, x->count, x->buf)
                                          : read_sectors(x->lba, x->count, x->buf));
            }
        }
        for (int j = 0; j < k; j++) if (!io[i + j].ok) all = 0;
        i += k;
    }
    kmutex_unlock(&g_ahci_lock);
    return all;
}

uint64_t ahci_ncq_rounds(void) { return g_ncq_rounds; }
uint64_t ahci_ncq_cmds(void) { return g_ncq_cmds; }
uint64_t ahci_ncq_fallbacks(void) { return g_ncq_fallbacks; }

// TRIM, through DATA SET MANAGEMENT. The range list travels DEVICE-WARD,
// so this is a WRITE-direction transfer -- getting that backwards is
// silent: the drive acknowledges a command whose payload never arrived
// and nothing is discarded (ata.c's DSM comment has the long version of
// that failure, which cost a build there).
static int trim_ranges(const struct blk_range *r, int n) {
    if (!ahci_trim_supported() || n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if (r[i].lba > g_sectors || r[i].count > g_sectors - r[i].lba) return 0;

    int ri = 0;
    uint32_t done = 0;
    // Packed straight into the bounce buffer, so this does NOT go through
    // the usual copy -- `count` here means descriptor BLOCKS, not
    // sectors, which is DSM's own meaning for the field.
    while (blk_dsm_pack(g_buf, r, n, &ri, &done))
        if (!run_command(ATA_DSM, DSM_FEATURE_TRIM, 0, 1, AHCI_SECTOR_SIZE, 1)) return 0;
    return 1;
}

int ahci_trim_ranges(const struct blk_range *r, int n) {
    kmutex_lock(&g_ahci_lock);
    int ok = trim_ranges(r, n);
    kmutex_unlock(&g_ahci_lock);
    return ok;
}

int ahci_trim(uint32_t lba, uint32_t count) {
    if (count == 0) return 0;
    struct blk_range one = { lba, count };
    return ahci_trim_ranges(&one, 1);
}

int ahci_trim_supported(void) { return ahci_present() && g_trim; }

static int flush(void) {
    if (!ahci_present()) return 0;
    return run_command(ATA_FLUSH_EXT, 0, 0, 0, 0, 0);
}

int ahci_flush(void) {
    kmutex_lock(&g_ahci_lock);
    int r = flush();
    kmutex_unlock(&g_ahci_lock);
    return r;
}

// ---- diagnostics -----------------------------------------------------

int ahci_controller_present(void) { return g_abar != 0; }
uint32_t ahci_version(void) { return g_abar ? hba_r(HBA_VS) : 0; }
uint32_t ahci_capabilities(void) { return g_cap; }
int ahci_command_slots(void) { return g_abar ? (int)(((g_cap >> CAP_NCS_SHIFT) & CAP_NCS) + 1) : 0; }
int ahci_active_port(void) { return g_active >= 0 ? g_ports[g_active].port : -1; }
uint8_t ahci_irq_line(void) { return g_irq; }
// EITHER kind of interrupt. Asking `g_irq != 0` alone reported a
// controller on a vector as POLLED -- the same shape as the
// input_source.irq/msi_vector conflation in docs/conventions/kernel.md.
int ahci_irq_driven(void) { return g_irq != 0 || g_msi_vector != 0; }
uint64_t ahci_cmd_sleeps(void) { return g_cmd_sleeps; }
int ahci_lba48(void) { return g_lba48; }
const char *ahci_model(void) { return g_model; }
int ahci_port_count(void) { return g_port_count; }

int ahci_port_status(int index, struct ahci_port_status *out) {
    if (index < 0 || index >= g_port_count || !out) return 0;
    *out = g_ports[index];
    return 1;
}
PCI_DRIVER("ahci", ahci_matches, ahci_probe);
