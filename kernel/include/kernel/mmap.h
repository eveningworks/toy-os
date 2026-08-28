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

#endif
