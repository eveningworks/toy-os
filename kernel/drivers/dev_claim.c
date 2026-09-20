// The device claim table -- kernel/include/kernel/dev_claim.h.
// driver-none: records who holds a device; drives nothing itself
//
// SYS_DEV_CLAIM / SYS_DEV_RELEASE, and the state behind them. Stage 1
// (SYS_DEV_MAP_BAR) could only ever grant a device NO ring-0 driver
// wanted; this is what makes the kernel let go of one it had.
#include "dev_claim.h"
#include "pci.h"
#include "pci_driver.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "scheduler.h"
#include "errno.h"
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
static struct { uint64_t pml4; int pid; } g_claims[PCI_MAX_DEVICES];

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

int dev_claim_drop(int index, uint64_t pml4, int rebind) {
    if (!valid(index)) return -EINVAL;
    if (!g_claims[index].pml4 || g_claims[index].pml4 != pml4) return -EACCES;

    int pid = g_claims[index].pid;
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
        g_claims[i].pml4 = 0;
        g_claims[i].pid  = 0;
    }
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
