#ifndef KERNEL_MMAP_H
#define KERNEL_MMAP_H

// SYS_MMAP's engine: the region list in struct sched_mm, the two
// syscall handlers (declared in syscalls.h with the others), and the
// fault-in below. The mappings themselves are reservations -- see
// uaddr.h's mmap-arena section for where they live and why.

#include <stdint.h>
#include "vmm.h"

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

// A claimed device's DMA region, unmapped and its slot freed. Called
// from dev_claim_drop() before the frames go back to the allocator,
// because they are BORROWED and no mapping teardown disposes of them
// (kernel/drivers/dev_claim.c). A no-op when the caller has no such
// region -- a KTEST claims from the kernel context and has no mm.
void mmap_drop_dma_region(uint64_t base, uint64_t npages);

// A DMA GRANT, in two halves, shared by the PCI and USB paths.
// reserve() picks an address and checks a region slot is free before
// anything is allocated; map() maps the frames UC and NX and records
// the region so teardown unmaps it. The frames belong to whoever
// allocated them -- these BORROW.
uint64_t mmap_dma_reserve(uint64_t npages);
int mmap_map_dma(uint64_t pml4, uint64_t base, uint64_t phys, uint64_t npages,
                 int writable, enum vmm_memtype mt);

#endif

// THE REGION LIST IS ALLOCATED (api/scheduler.h), so exactly one place
// must free it and one must not share it.
//
// `mmap_release_regions` is that place: called from
// release_process_state() (kernel/proc/syscall.c), which both the exit
// path and the kill-from-outside path already funnel through. It is
// idempotent, so a spawn reusing a slot calls it too rather than
// trusting that a previous teardown ran.
void mmap_release_regions(uint64_t pml4_phys);
void mmap_regions_reset(struct sched_mm *mm);

// A fork copies `struct sched_mm` by value, pointer included. This
// gives the child its own copy; 0 means the allocation failed and the
// child must not be started, because the alternative is two owners of
// one array.
int mmap_clone_regions(struct sched_mm *dst, const struct sched_mm *src);

// mm_test.c only: free_slot(), which is where the list grows.
struct mmap_region *mmap_test_free_slot(struct sched_mm *mm);
