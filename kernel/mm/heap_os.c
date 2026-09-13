// The kernel's half of the shared allocator -- see api/heap_os.h.
//
// Memory comes from the physical frame allocator directly, with no
// mapping step: every managed frame is identity-mapped (boot.asm below
// 4 GiB, paging_extend_identity_map() above), so a frame
// pmm_alloc_contiguous() hands back is already a valid kernel pointer.
// That is why the kernel heap needs no vmm involvement at all.
//
// PMM_ZONE_ANY, so on a big machine the heap lives above 4 GiB and the
// low zone is left for DMA. THE TRAP: kmalloc memory is therefore NOT
// a DMA target for a 32-bit engine -- a driver with a 32-bit address
// register takes DMA32 frames from pmm, never a kmalloc buffer.
#include "heap_os.h"
#include "pmm.h"
#include "klog.h"
#include "fault_inject.h"
#include "scheduler.h" // preempt_disable/_enable -- see heap_os_lock() below

#define HEAP_PAGE_SIZE 4096

void *heap_os_alloc(uint64_t bytes) {
    uint64_t pages = (bytes + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
    // CONTIGUOUS, not a page at a time: the allocator hands out blocks
    // that span pages, so a region has to be one run.
    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    return phys ? (void *)(uintptr_t)phys : 0;
}

void heap_os_report(const char *msg) { klog_write(msg); }

int heap_os_should_fail_alloc(void) { return fault_should_fail_alloc(); }

// PREEMPTION OFF, NOT A LOCK -- and the day these stopped being no-ops
// came before SMP, which is what the old comment here predicted.
// Making the syscall gate a TRAP gate is what falsifies "nothing
// preempts kernel code mid-kmalloc": a timer tick lands inside a
// syscall, switches to a process that also allocates, and two walkers
// share one free list. bounce_alloc() is on every read and write, so
// this is the hot path rather than a corner.
//
// scheduler_preempt_disable() is enough BECAUSE NO INTERRUPT HANDLER
// ALLOCATES -- measured across all eleven registered IRQ handlers, not
// assumed. That is the invariant to keep: an IRQ handler that starts
// calling kmalloc() re-enters the list the guard is protecting, and
// the guard cannot see it. Such a handler would need the allocator to
// mask interrupts instead (Linux's spin_lock_irqsave), which costs
// every allocation in the kernel to serve one caller.
//
// Balanced by construction: heap_core.c's kmalloc/kfree are thin
// wrappers around unlocked inner functions precisely so no early
// return can skip the unlock -- an unbalanced disable() hangs the
// machine (api/scheduler.h).
void heap_os_lock(void) { scheduler_preempt_disable(); }
void heap_os_unlock(void) { scheduler_preempt_enable(); }
