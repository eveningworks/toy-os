#ifndef KERNEL_DEV_CLAIM_H
#define KERNEL_DEV_CLAIM_H

#include <stdint.h>

// WHICH PROCESS HAS TAKEN A PCI DEVICE -- docs/umdf-design.md stage 2,
// and the other half of pci_driver.h's `g_bound[]`: that table says
// which ring-0 driver was handed a device, this one says which ring-3
// process holds it instead. Both are keyed by the enumeration index.
//
// The claim is DEVICE state rather than process state, which is why it
// lives beside the binding and not in an fd. The fd is what vfio-pci
// uses and it remains the exit if a claim ever has to be passed between
// processes -- see docs/decisions.md.
//
// A CLAIM IS NOT CONTAINMENT. The kernel's identity map still covers a
// BAR below 4 GiB, and without an IOMMU a claimed card can be pointed
// at any physical page. What a claim buys is that exactly one driver
// programs a register file, and that a crash takes a process rather
// than the machine.

// Takes the claim for `pml4`, releasing any ring-0 driver first.
// Re-claiming by the same address space is idempotent.
//   -EINVAL  no such device
//   -EPERM   `pml4` is 0 -- the caller has no address space
//   -EBUSY   another address space holds it
//   -ENOTSUP a ring-0 driver is bound and has no remove()
int dev_claim_take(int index, uint64_t pml4, int pid);

// Drops it. With `rebind`, the bus re-probes the device so its ring-0
// driver takes it back; without, the device stays unbound -- vfio-pci's
// behaviour, and what lets a supervised driver restart and re-claim.
//   -EINVAL  no such device
//   -EACCES  `pml4` does not hold it
int dev_claim_drop(int index, uint64_t pml4, int rebind);

// 0 when this address space does not hold the device: -EACCES when
// nobody does, -EBUSY when somebody else does. What dev_bar_check()
// asks, so every grant inherits it.
int dev_claim_check(int index, uint64_t pml4);

// The holder's pid, or 0. `lspci`'s column.
int dev_claim_holder_pid(int index);

// Every claim an address space holds, dropped without a rebind. Called
// from release_process_state() -- a claim is one more thing keyed to a
// dying address space that no mapping teardown would reach.
void dev_claim_space_gone(uint64_t pml4);

#endif
