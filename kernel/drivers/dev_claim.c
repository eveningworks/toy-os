// The device claim table -- kernel/include/kernel/dev_claim.h.
// driver-none: records who holds a device; drives nothing itself
//
// SYS_DEV_CLAIM / SYS_DEV_RELEASE, and the state behind them. Stage 1
// (SYS_DEV_MAP_BAR) could only ever grant a device NO ring-0 driver
// wanted; this is what makes the kernel let go of one it had.
#include "dev_claim.h"
#include "pci.h"
#include "pci_driver.h"
#include "pci_internal.h" // pci_command_update -- the bus-master bit
#include "pmm.h"
#include "irq.h"
#include "futex.h"   // futex_note_ready -- the holder's wakeword
#include "mmap.h" // mmap_drop_dma_region -- the holder's view, dropped first
#include "syscalls.h"
#include "syscall_abi.h"
#include "scheduler.h"
#include "errno.h"
#include "io.h"     // inb/outb -- the whole reason SYS_DEV_IO exists
#include "vmm.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"
#include "vmm.h"
#include "query.h"
#include "query_abi.h"
#include "initcall.h"
#include "string.h"

// Keyed by the enumeration index, like g_bound[]. OCCUPANCY IS THE
// PML4, NOT THE PID: the kernel context has no pid and a KTEST claims
// from it, so a pid of 0 is a legitimate holder.
static void dev_irq_release(int index);   // defined with the stub below

static struct {
    uint64_t pml4;
    int pid;
    // The DMA buffer, if this holder asked for one. It belongs to the
    // CLAIM: a borrowed mapping of it dies with the address space, and
    // these frames are freed here, after bus mastering is off.
    uint64_t dma_phys;
    uint64_t dma_pages;
    uint64_t dma_base;   // where the holder has it mapped, 0 if nowhere
} g_claims[PCI_MAX_DEVICES];

static int valid(int index) {
    return index >= 0 && index < pci_device_count() && index < PCI_MAX_DEVICES;
}

int dev_claim_holder_pid(int index) {
    if (!valid(index) || !g_claims[index].pml4) return 0;
    return g_claims[index].pid;
}

int dev_claim_check(int index, uint64_t pml4) {
    if (!valid(index)) return -EINVAL;
    if (!g_claims[index].pml4) return -EACCES;
    return g_claims[index].pml4 == pml4 ? 0 : -EBUSY;
}

int dev_claim_take(int index, uint64_t pml4, int pid) {
    if (!valid(index)) return -EINVAL;
    if (!pml4) return -EPERM;
    if (g_claims[index].pml4)
        return g_claims[index].pml4 == pml4 ? 0 : -EBUSY;

    // The unbind, and it comes before the claim is recorded so a driver
    // that cannot let go leaves the table exactly as it was.
    const struct pci_device *d = pci_device_at(index);
    if (pci_device_driver(d)) {
        int r = pci_device_release(index);
        if (r < 0) return r;
    }

    g_claims[index].pml4 = pml4;
    g_claims[index].pid  = pid;
    klog_printf("dev: pid %d claimed pci %d (%02x:%02x.%u)\n",
                pid, index, d->bus, d->device, d->function);
    return 0;
}

int dev_claim_dma_take(int index, uint64_t pml4, uint64_t pages, uint64_t base,
                       uint64_t *phys_out) {
    int held = dev_claim_check(index, pml4);
    if (held != 0) return held;
    if (!pages || pages > DEV_DMA_MAX_BYTES / 4096) return -EINVAL;
    if (g_claims[index].dma_phys) return -EBUSY;

    // DMA32: a device that takes a 32-bit address must be able to reach
    // it, and this kernel identity-maps below 4 GiB so it can be zeroed
    // here rather than by the holder.
    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_DMA32);
    if (!phys) return -ENOMEM;
    k_memset((void *)(uintptr_t)phys, 0, (uint32_t)(pages * 4096));
    // TWO MEMORY TYPES FOR ONE PAGE, so the dirty lines this just made
    // have to go. The kernel zeroes through the identity map, which is
    // WB; the holder maps the same frames UC. A WB line evicted after
    // ring 3 has written a descriptor through the UC alias silently
    // reverts it in RAM, and the card fetches what the CPU wrote
    // earlier -- invisible under TCG, which ignores PAT. paging.c does
    // the same on every framebuffer retype.
    __asm__ volatile ("wbinvd" ::: "memory");

    g_claims[index].dma_phys  = phys;
    g_claims[index].dma_pages = pages;
    g_claims[index].dma_base  = base;
    pci_command_update(pci_device_at(index), PCI_CMD_BUS_MASTER, 0);
    if (phys_out) *phys_out = phys;
    klog_printf("dev: pid %d dma %llu page(s) at %llx on pci %d, bus master on\n",
                g_claims[index].pid, (unsigned long long)pages,
                (unsigned long long)phys, index);
    return 0;
}

// Bus mastering OFF, then the frames back. The order is the whole
// point: a card still mastering the bus over freed frames writes into
// whatever the allocator hands out next, and nothing would connect the
// corruption to the driver that died.
// `unmap` is 0 only from dev_claim_space_gone(), where the address
// space is already being torn down and there is nothing left to unmap.
static void dma_release(int index, int unmap) {
    if (!g_claims[index].dma_phys) return;
    pci_command_update(pci_device_at(index), 0, PCI_CMD_BUS_MASTER);
    // THE MAPPING GOES FIRST. The frames are borrowed, so no mapping
    // teardown disposes of them -- which means an explicit release
    // would otherwise hand them to the allocator while the caller still
    // holds a live writable PTE for every one, and the next owner of
    // those frames shares them with a process that can still write.
    // `meminfo audit` calls exactly that a DANGLING mapping.
    if (unmap) mmap_drop_dma_region(g_claims[index].dma_base,
                                    g_claims[index].dma_pages);
    pmm_free_contiguous(g_claims[index].dma_phys, g_claims[index].dma_pages);
    g_claims[index].dma_phys = g_claims[index].dma_pages = 0;
    g_claims[index].dma_base = 0;
}

int dev_claim_dma_drop(int index, uint64_t pml4) {
    int held = dev_claim_check(index, pml4);
    if (held != 0) return held;
    dma_release(index, 1);
    return 0;
}

int dev_claim_drop(int index, uint64_t pml4, int rebind) {
    if (!valid(index)) return -EINVAL;
    if (!g_claims[index].pml4 || g_claims[index].pml4 != pml4) return -EACCES;

    int pid = g_claims[index].pid;
    dev_irq_release(index);
    dma_release(index, 1);
    g_claims[index].pml4 = 0;
    g_claims[index].pid  = 0;
    klog_printf("dev: pid %d released pci %d%s\n", pid, index,
                rebind ? " (rebinding)" : "");
    if (rebind) pci_device_rebind(index);
    return 0;
}

void dev_claim_space_gone(uint64_t pml4) {
    if (!pml4) return;
    for (int i = 0; i < PCI_MAX_DEVICES; i++) {
        if (g_claims[i].pml4 != pml4) continue;
        // NO REBIND. A driver under supervision is restarted and has to
        // find the device still free; taking it back here would race
        // that restart, and the ring-0 driver would win about half the
        // time. `SYS_DEV_RELEASE` with DEV_RELEASE_REBIND is how a
        // device goes back deliberately.
        klog_printf(KLOG_WARN "dev: pid %d died holding pci %d -- left unbound\n",
                    g_claims[i].pid, i);
        // The mapping of these frames died with the address space, but
        // the DEVICE did not stop: dma_release() clears bus mastering
        // before they go back to the allocator.
        dev_irq_release(i);
        dma_release(i, 0);
        g_claims[i].pml4 = 0;
        g_claims[i].pid  = 0;
    }
}

// --- a claimed device's interrupt --------------------------------------
//
// abi/syscall_abi.h carries the contract. What is here is the stub that
// runs in INTERRUPT CONTEXT and the bookkeeping behind it.
//
// FOUR SLOTS AND A TRAMPOLINE EACH, because an irq_handler_fn is handed
// only the register frame -- not the line it is being called for -- so
// one shared stub could not tell which claimed device fired. hda.c
// solves the same problem the same way for its two controllers.
#define DEV_IRQ_SLOTS 4

static struct {
    int      used;     // 0 = free, which is what a static zero gives --
                       // no initcall to establish a sentinel, and the
                       // same "zero means nobody" the claim table uses
    int      index;    // the PCI device
    uint8_t  line;     // INTx, 0 when this is MSI
    uint8_t  vector;   // MSI, 0 when this is INTx
    uint32_t count;    // interrupts since the last ack
    int      masked;
} g_irq[DEV_IRQ_SLOTS];

static void dev_irq_fire(int slot) {
    if (slot < 0 || slot >= DEV_IRQ_SLOTS || !g_irq[slot].used) return;
    // THE MASK FIRST, and before anything that could take time: a level
    // line re-asserts the instant this returns otherwise, and the
    // machine never leaves the handler.
    if (g_irq[slot].line && !g_irq[slot].masked) {
        irq_mask(g_irq[slot].line);
        g_irq[slot].masked = 1;
    }
    g_irq[slot].count++;
    // The holder's wakeword: bumped and woken from interrupt context,
    // which win_input.c already does from the input IRQ.
    futex_note_ready(dev_claim_holder_pid(g_irq[slot].index));
}

#define DEV_IRQ_STUB(n) static void dev_irq_stub##n(uint64_t *regs) { \
    (void)regs; dev_irq_fire(n); }
DEV_IRQ_STUB(0)
DEV_IRQ_STUB(1)
DEV_IRQ_STUB(2)
DEV_IRQ_STUB(3)
static const irq_handler_fn g_irq_stub[DEV_IRQ_SLOTS] = {
    dev_irq_stub0, dev_irq_stub1, dev_irq_stub2, dev_irq_stub3,
};

static int irq_slot_of(int index) {
    for (int i = 0; i < DEV_IRQ_SLOTS; i++)
        if (g_irq[i].used && g_irq[i].index == index) return i;
    return -1;
}

// Everything the arm did, undone. Called when the claim drops, so a
// rebound ring-0 driver finds its line unmasked and unhandled.
static void dev_irq_release(int index) {
    int slot = irq_slot_of(index);
    if (slot < 0) return;
    const struct pci_device *d = pci_device_at(index);
    if (g_irq[slot].vector) {
        pci_msi_release(d, g_irq[slot].vector);
    } else if (g_irq[slot].line) {
        irq_unregister_handler(g_irq[slot].line, g_irq_stub[slot]);
        // UNMASKED ON THE WAY OUT. A line left masked is one the next
        // driver to bind this device never hears from, and nothing
        // about a dead holder should be inherited.
        irq_unmask(g_irq[slot].line);
    }
    k_memset(&g_irq[slot], 0, sizeof g_irq[slot]);   // used = 0: free
}

int dev_claim_irq_enable(int index, uint64_t pml4) {
    int held = dev_claim_check(index, pml4);
    if (held != 0) return held;
    if (irq_slot_of(index) >= 0) return -EBUSY;
    // WITHOUT A WAKEWORD THE BUMP HAS NOWHERE TO LAND, and the driver
    // would park forever on a word nothing writes. Refused here rather
    // than discovered as a hang.
    if (!futex_wakeword_phys(dev_claim_holder_pid(index))) return -ENODEV;

    int slot = -1;
    for (int i = 0; i < DEV_IRQ_SLOTS; i++)
        if (!g_irq[i].used) { slot = i; break; }
    if (slot < 0) return -ENOSPC;

    const struct pci_device *d = pci_device_at(index);
    k_memset(&g_irq[slot], 0, sizeof g_irq[slot]);
    g_irq[slot].used = 1;
    g_irq[slot].index = index;

    uint8_t vec = pci_msi_request(d, g_irq_stub[slot]);
    if (vec) {
        g_irq[slot].vector = vec;      // edge triggered: nothing to mask
    } else {
        uint8_t line = pci_irq_line(d);
        if (line == 0xFF || line == 0 || line >= 16) {
            g_irq[slot].used = 0;
            return -ENOTSUP;
        }
        g_irq[slot].line = line;
        irq_register_handler(line, g_irq_stub[slot]);
        pci_command_update(d, 0, PCI_CMD_INTX_DISABLE);
        irq_unmask(line);
    }
    klog_printf("dev: pci %d irq -> pid %d (%s)\n", index,
                dev_claim_holder_pid(index),
                g_irq[slot].vector ? "msi" : "intx");
    return 0;
}

int dev_claim_irq_ack(int index, uint64_t pml4) {
    int held = dev_claim_check(index, pml4);
    if (held != 0) return held;
    int slot = irq_slot_of(index);
    if (slot < 0) return -EINVAL;
    uint32_t n = g_irq[slot].count;
    g_irq[slot].count = 0;
    if (g_irq[slot].masked) {
        g_irq[slot].masked = 0;
        irq_unmask(g_irq[slot].line);   // the device is quiet: let it speak
    }
    return (int)n;
}

// --- syscalls ----------------------------------------------------------

int sys_dev_claim(struct syscall_ctx *c) {
    int index = (int)(int64_t)c->a0;
    int64_t ret = scheduler_current_mm()
        ? dev_claim_take(index, c->pml4, scheduler_current_pid())
        : -EPERM; // the legacy `run` loader: no slot, so nothing to key on
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_dev_release(struct syscall_ctx *c) {
    int index = (int)(int64_t)c->a0;
    int rebind = (int)(c->a1 & DEV_RELEASE_REBIND);
    int64_t ret = scheduler_current_mm()
        ? dev_claim_drop(index, c->pml4, rebind)
        : -EPERM;
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_dev_irq_enable(struct syscall_ctx *c) {
    int index = (int)(int64_t)c->a0;
    int64_t ret = scheduler_current_mm()
        ? dev_claim_irq_enable(index, c->pml4) : -EPERM;
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// PORT I/O ON A CLAIMED DEVICE -- syscall_abi.h says why this exists
// and why it is not the TSS bitmap.
//
// THE VALIDATION IS THE WHOLE POINT. A holder names a BAR and an
// offset, never a port, and the offset is checked against that BAR's
// own size -- so this can never reach a port belonging to another
// device, which `ioperm()` handing over a range could.
int sys_dev_io(struct syscall_ctx *c) {
    struct dev_io_msg m;
    int64_t ret;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = dev_claim_check((int)m.index, c->pml4)) != 0) goto out;
    if (m.bar >= 6 || (m.width != 1 && m.width != 2 && m.width != 4)) {
        ret = -EINVAL;
        goto out;
    }

    const struct pci_device *d = pci_device_at((int)m.index);
    if (!d || !pci_bar_is_io(d->bar[m.bar])) { ret = -EINVAL; goto out; }
    // The offset AND the access's width must both be inside: a 4-byte
    // read one byte from the end would otherwise walk off it.
    uint64_t size = d->bar_size[m.bar];
    if (!size || (uint64_t)m.offset + m.width > size) { ret = -EINVAL; goto out; }

    uint16_t port = (uint16_t)(pci_bar_addr(d->bar[m.bar]) + m.offset);
    if (m.write) {
        if (m.width == 1) outb(port, (uint8_t)m.value);
        else if (m.width == 2) outw(port, (uint16_t)m.value);
        else outl(port, m.value);
        ret = 0;
    } else {
        m.value = m.width == 1 ? inb(port) : m.width == 2 ? inw(port) : inl(port);
        ret = vmm_copy_to_user(c->pml4, c->a0, &m, sizeof m) ? 0 : -EFAULT;
    }
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_dev_irq_ack(struct syscall_ctx *c) {
    int index = (int)(int64_t)c->a0;
    int64_t ret = scheduler_current_mm()
        ? dev_claim_irq_ack(index, c->pml4) : -EPERM;
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// --- QUERY_PCIDEV ------------------------------------------------------
//
// The only reader of dev_claim_holder_pid() outside a test, and the
// reason the claim is visible at all from ring 3: `lspci -k`.

static int pcidev_count(void) {
    int n = pci_device_count();
    return n > PCI_MAX_DEVICES ? PCI_MAX_DEVICES : n;
}

static int pcidev_fill(int index, void *out) {
    if (!valid(index)) return 0;
    struct query_pcidev *q = out;
    k_memset(q, 0, sizeof *q);
    q->index = (uint32_t)index;
    q->holder_pid = dev_claim_holder_pid(index);
    const char *drv = pci_device_driver(pci_device_at(index));
    // What dev_claim_take() would allow, not just what the bus calls
    // removable: an UNBOUND device has no driver to ask for a remove().
    q->claimable = (!drv || pci_device_removable(index)) ? 1 : 0;
    if (drv) k_strlcpy(q->driver, drv, sizeof q->driver);
    return 1;
}

static const struct query_provider pcidev_provider = {
    .cls = QUERY_PCIDEV,
    .name = "pcidev",
    .record_size = sizeof(struct query_pcidev),
    .flags = QUERY_F_LIST,
    .count = pcidev_count,
    .fill = pcidev_fill,
};

static void dev_claim_query_init(void) { query_register(&pcidev_provider); }
INITCALL(dev_claim_query_init, INIT_QUERY);

// --- KTESTs ------------------------------------------------------------
//
// They claim from the KERNEL's address space, which is exactly the path
// ring 3 takes minus the syscall wrapper -- and the only way to test it
// at all, since a KTEST has no process of its own.

// A device no driver wants, so a KTEST never unbinds a live one.
static int unbound_device(void) {
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++)
        if (!pci_device_driver(pci_device_at(i)) && !dev_claim_holder_pid(i))
            return i;
    return -1;
}

KTEST("dev_claim", "a claim is exclusive, idempotent and refuses a stranger") {
    int i = unbound_device();
    if (i < 0) { KTEST_SKIP("no unbound PCI device on this machine"); return; }
    uint64_t me = vmm_current_pml4();

    KTEST_ASSERT_EQ(dev_claim_check(i, me), -EACCES);   // nobody holds it
    KTEST_ASSERT_EQ(dev_claim_take(i, me, 0), 0);
    KTEST_ASSERT_EQ(dev_claim_take(i, me, 0), 0);       // idempotent
    KTEST_ASSERT_EQ(dev_claim_check(i, me), 0);

    // A different address space: refused, and told apart from "free".
    uint64_t other = me + 0x1000;
    KTEST_ASSERT_EQ(dev_claim_take(i, other, 7), -EBUSY);
    KTEST_ASSERT_EQ(dev_claim_check(i, other), -EBUSY);
    KTEST_ASSERT_EQ(dev_claim_drop(i, other, 0), -EACCES);

    KTEST_ASSERT_EQ(dev_claim_drop(i, me, 0), 0);
    KTEST_ASSERT_EQ(dev_claim_check(i, me), -EACCES);
}

KTEST("dev_claim", "a driver with no remove() can never be claimed") {
    // The whole gate: this kernel has no uid, so a driver CAPABILITY
    // stands in for a privilege check. Nothing carrying the root
    // filesystem has a remove(), so nothing can take it.
    //
    // It ASKS pci_device_removable() rather than probing by claiming:
    // a probe would unbind whatever IS removable -- the sound card on a
    // machine that has one -- to learn something the bus already knows,
    // and a KTEST runs with a desktop up.
    int found = 0;
    uint64_t me = vmm_current_pml4();
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!pci_device_driver(d) || pci_device_removable(i)) continue;
        KTEST_ASSERT_EQ(dev_claim_take(i, me, 0), -ENOTSUP);
        KTEST_ASSERT_EQ(dev_claim_check(i, me), -EACCES); // nothing recorded
        KTEST_ASSERT(pci_device_driver(d) != 0);          // still bound
        found = 1;
    }
    if (!found) { KTEST_SKIP("every bound driver here is removable"); return; }
}

KTEST("dev_claim", "NOTHING CARRYING THE ROOT FILESYSTEM IS RELEASABLE") {
    // What the gate admits, asserted directly rather than assumed. The
    // removable drivers are `hda` and the two NICs -- a MODULE must
    // have a remove() to be unloadable at all, so e1000 and r8169 were
    // removable long before this existed. A storage controller is not,
    // and that is the property worth a test: the class that carries
    // the root must never be claimable out from under the filesystem.
    //
    // The gate is also only meaningful if SOMETHING passes it, so a
    // machine where nothing does SKIPS rather than passing quietly.
    int removable = 0, bound = 0;
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!pci_device_driver(d)) continue;
        bound++;
        if (!pci_device_removable(i)) continue;
        removable++;
        KTEST_ASSERT(d->class_code != 0x01);  // mass storage
    }
    if (!bound) { KTEST_SKIP("nothing bound on this machine"); return; }
    if (!removable) { KTEST_SKIP("no driver here has a remove()"); return; }
}

KTEST("dev_claim", "an unknown device and a claimless address space") {
    KTEST_ASSERT_EQ(dev_claim_take(-1, vmm_current_pml4(), 0), -EINVAL);
    KTEST_ASSERT_EQ(dev_claim_take(PCI_MAX_DEVICES, vmm_current_pml4(), 0), -EINVAL);
    KTEST_ASSERT_EQ(dev_claim_take(0, 0, 0), -EPERM);
    KTEST_ASSERT_EQ(dev_claim_holder_pid(-1), 0);
    dev_claim_space_gone(0);   // must not walk the table for a null space
}

KTEST("dev_claim", "a dying address space drops every claim it held") {
    int i = unbound_device();
    if (i < 0) { KTEST_SKIP("no unbound PCI device on this machine"); return; }
    uint64_t ghost = vmm_current_pml4() + 0x2000;   // a space nothing runs in
    KTEST_ASSERT_EQ(dev_claim_take(i, ghost, 99), 0);
    KTEST_ASSERT_EQ(dev_claim_holder_pid(i), 99);
    dev_claim_space_gone(ghost);
    KTEST_ASSERT_EQ(dev_claim_holder_pid(i), 0);
    KTEST_ASSERT_EQ(dev_claim_check(i, ghost), -EACCES);
}
