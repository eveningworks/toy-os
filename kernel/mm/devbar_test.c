// SYS_DEV_MAP_BAR's validation, and the memory type a register file
// gets. Stage 1 of docs/umdf-design.md.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS**, which is what these are
// shaped around:
//
//   - A grant that ignored the memory type would pass every check about
//     addresses and sizes, and produce a CACHEABLE register file --
//     stores sitting in a line until something else evicts them, which
//     reads as a device that works on one machine and hangs on another.
//     So the type is asserted directly, through vmm_user_memtype().
//   - A check that refused everything would pass every REFUSAL test
//     here. So the last one requires a real unbound memory BAR to be
//     ACCEPTED, and says plainly when the machine has none rather than
//     reporting a pass it did not earn.
//
// These run on the kernel context: the syscall refuses that at its
// first line, which is exactly why the validation is a function of its
// own (kernel/syscalls.h). The kernel's address space can still hold a
// CLAIM, so the accepting test below takes one the way ring 3 does.
#include "ktest.h"
#include "syscalls.h"
#include "errno.h"
#include "pci.h"
#include "pci_internal.h"
#include "pci_driver.h"
#include "vmm.h"
#include "pmm.h"
#include "dev_claim.h"

#define TEST_VADDR 0x9100000000ULL // clear of uaccess_test's own space

KTEST("devbar", "a BAR index outside the enumeration is refused") {
    uint64_t phys = 0, npages = 0, me = vmm_current_pml4();
    KTEST_ASSERT(dev_bar_check(-1, 0, me, &phys, &npages) == -EINVAL);
    KTEST_ASSERT(dev_bar_check(pci_device_count(), 0, me, &phys, &npages) == -EINVAL);
    // Six BARs exist; the seventh is a read off the end of the struct.
    KTEST_ASSERT(dev_bar_check(0, 6, me, &phys, &npages) == -EINVAL);
    KTEST_ASSERT(dev_bar_check(0, -1, me, &phys, &npages) == -EINVAL);
}

KTEST("devbar", "a device a ring-0 driver holds is refused, and an I/O BAR too") {
    int saw_bound = 0, saw_io = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;
        for (int b = 0; b < 6; b++) {
            int rc = dev_bar_check(i, b, vmm_current_pml4(), 0, 0);
            if (pci_bar_is_io(d->bar[b])) {
                // An I/O BAR is ENOTSUP whoever holds the device: port
                // I/O needs a permission model this kernel has not got,
                // and there is no physical page to hand over.
                KTEST_ASSERT(rc == -ENOTSUP);
                saw_io = 1;
            } else if (pci_device_driver(d) && pci_bar_addr(d->bar[b]) &&
                       pci_bar_mem_size(d, b)) {
                // A real memory BAR on a bound device: EBUSY, never a
                // grant. This is vfio-pci's unbind-first rule.
                KTEST_ASSERT(rc == -EBUSY);
                saw_bound = 1;
            }
        }
    }
    // SKIPPED, not passed, when this machine had neither to look at:
    // "no device matched" and "every device passed" are identical in a
    // green run, and only one of them is evidence.
    if (!saw_bound && !saw_io)
        KTEST_SKIP("no bound device with a memory BAR, and no I/O BAR");
}

KTEST("devbar", "an unbound memory BAR is ACCEPTED once it is CLAIMED") {
    int granted = 0;
    uint64_t me = vmm_current_pml4();
    for (int i = 0; i < pci_device_count() && !granted; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d || pci_device_driver(d) || dev_claim_holder_pid(i)) continue;
        if (dev_claim_take(i, me, 0) != 0) continue;
        for (int b = 0; b < 6; b++) {
            uint64_t phys = 0, npages = 0;
            if (dev_bar_check(i, b, me, &phys, &npages) != 0) continue;
            // The check's own contract: what it fills is the BAR's
            // base and the pages its PROBED size covers, not a guess.
            KTEST_ASSERT(phys == pci_bar_addr(d->bar[b]));
            KTEST_ASSERT((phys & 0xFFF) == 0);
            KTEST_ASSERT(npages == (pci_bar_mem_size(d, b) + 4095) / 4096);
            KTEST_ASSERT(npages > 0);
            // THE SAME DEVICE AND BAR, refused for the two reasons
            // stage 2 added -- so what changed is the CLAIM and not
            // some property of the device. A stranger's address space
            // is EBUSY; no address space at all is EPERM.
            KTEST_ASSERT(dev_bar_check(i, b, me + 0x1000, 0, 0) == -EBUSY);
            KTEST_ASSERT(dev_bar_check(i, b, 0, 0, 0) == -EPERM);
            dev_claim_drop(i, me, 0);
            KTEST_ASSERT(dev_bar_check(i, b, me, 0, 0) == -EACCES);
            KTEST_ASSERT(dev_claim_take(i, me, 0) == 0);
            granted = 1;
            break;
        }
        dev_claim_drop(i, me, 1);
    }
    // THE POSITIVE CONTROL FOR EVERY REFUSAL ABOVE. Without it, a
    // dev_bar_check() returning -EINVAL unconditionally would leave
    // this whole suite green.
    if (!granted) KTEST_SKIP("no unbound memory BAR on this machine to grant");
}

KTEST("devbar", "a register mapping is UNCACHEABLE, and a normal one is not") {
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);
    uint64_t frame = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(frame != 0);

    // Borrowed and UC, the way a BAR is granted. The frame is ordinary
    // RAM here -- what is under test is the PTE, not the device.
    KTEST_ASSERT(vmm_map_user_borrowed(as, TEST_VADDR, frame, 1, 1, VMM_MT_UC));
    KTEST_ASSERT(vmm_user_memtype(as, TEST_VADDR) == VMM_MT_UC);

    // And the contrast, so the assertion above is not satisfied by a
    // reader that answers UC for everything.
    KTEST_ASSERT(vmm_map_user_borrowed(as, TEST_VADDR + 4096, frame, 1, 1,
                                       VMM_MT_NORMAL));
    KTEST_ASSERT(vmm_user_memtype(as, TEST_VADDR + 4096) == VMM_MT_NORMAL);

    // An address with nothing mapped answers NORMAL, not UC: a caller
    // asking "is this uncacheable?" must get a no.
    KTEST_ASSERT(vmm_user_memtype(as, TEST_VADDR + 8192) == VMM_MT_NORMAL);

    vmm_destroy_address_space(as);
    pmm_free_frame(frame); // borrowed: teardown did not take it
}
