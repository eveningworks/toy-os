// The kernel's half of the shared allocator -- see api/heap_os.h.
//
// Memory comes from the physical frame allocator directly, with no
// mapping step: boot.asm identity-maps the whole low 4 GiB as
// supervisor-only, so a DMA32 frame pmm_alloc_contiguous() hands back
// is already a valid kernel pointer. That is why the kernel heap needs
// no vmm involvement at all, and why it stays on PMM_ZONE_DMA32 until
// the identity map is extended (docs/roadmap.md's "More than 4 GiB of
// RAM", stage 3).
#include "heap_os.h"
#include "pmm.h"
#include "klog.h"
#include "fault_inject.h"

#define HEAP_PAGE_SIZE 4096

void *heap_os_alloc(uint64_t bytes) {
    uint64_t pages = (bytes + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
    // CONTIGUOUS, not a page at a time: the allocator hands out blocks
    // that span pages, so a region has to be one run.
    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_DMA32);
    return phys ? (void *)(uintptr_t)phys : 0;
}

void heap_os_report(const char *msg) { klog_write(msg); }

int heap_os_should_fail_alloc(void) { return fault_should_fail_alloc(); }

// NO-OPS, and heap_os.h says why: nothing preempts kernel code between
// two instructions of kmalloc(). The day that stops being true is the
// day SMP lands, and `docs/smp-design.md` names this as the first lock
// to make real -- so the call sites already exist and only these two
// bodies change.
void heap_os_lock(void) { }
void heap_os_unlock(void) { }
