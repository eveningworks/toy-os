#ifndef KERNEL_MMAP_H
#define KERNEL_MMAP_H

// SYS_MMAP's engine: the region list in struct sched_mm, the two
// syscall handlers (declared in syscalls.h with the others), and the
// fault-in below. The mappings themselves are reservations -- see
// uaddr.h's mmap-arena section for where they live and why.

#include <stdint.h>

struct sched_mm;

// The mmap arena's slice of the demand-paging hook. proc_syscalls.c's
// uheap_fault() delegates any address inside the arena here; same
// contract as vmm_fault_fn: 1 = a page was mapped, retry the access;
// 0 = not this subsystem's address, or a page it could not produce --
// both stay fatal to the toucher.
int mmap_fault_in(struct sched_mm *mm, uint64_t pml4_phys, uint64_t vaddr);

// The two halves of a fork's view of the arena. `mmap_inherits_at` is
// vmm_fork_opts.inherit_borrowed: a borrowed leaf inside a SHM or FILE
// region is the child's too. `mmap_inherit_shm` takes the child's own
// reference on every SHM object `mm` maps, so its teardown drops one
// it holds; -ENOMEM leaves nothing taken.
int mmap_inherits_at(void *mm, uint64_t va);
int mmap_inherit_shm(uint64_t child_pml4, const struct sched_mm *mm);

#endif
