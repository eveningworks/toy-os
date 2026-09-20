// A process's mappings, as queryable FACTS -- what /bin/pmap prints.
//
// One flat list over EVERY live process rather than a per-pid class,
// because a list provider's index has nowhere to carry a second
// selector; the pid rides in the record and the client filters. Three
// records per process are SYNTHESIZED (image, heap, stack -- spans the
// region list never held) so the reader gets the whole address space
// from one class rather than gluing this to sched_mm's scalars itself.
#include "query.h"
#include "scheduler.h"
#include "proc_info.h"
#include "syscall_abi.h" // SYS_PROT_* -- a region's prot is reported raw
#include "uaddr.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"

#define IMAGE_BASE 0x8000000000ULL // userland/rt/link.ld's origin

// Records for one process: image, heap, stack, then its live regions.
static int records_of(struct sched_mm *mm) {
    int n = 3;
    if (!mm->regions) return 0;
    for (int i = 0; i < mm->region_cap; i++)
        if (mm->regions[i].base) n++;
    return n;
}

// The walk is re-run per fill() rather than cached: same
// count-then-index convention every list provider here uses, and the
// table can change between calls anyway -- a torn read across records
// is inherent to the interface and fine for a diagnostic.
static int procmap_count(void) {
    int n = 0;
    for (int pid = 1; pid <= SYS_PROC_MAX; pid++) {
        struct sched_mm *mm = scheduler_mm_for_pid(pid);
        if (mm) n += records_of(mm);
    }
    return n;
}

static void fill_one(struct query_procmap *q, int pid, struct sched_mm *mm,
                     int which) {
    k_memset(q, 0, sizeof *q);
    q->pid = (uint64_t)pid;
    if (which == 0) {
        q->kind  = QUERY_PROCMAP_IMAGE;
        q->base  = IMAGE_BASE;
        q->bytes = mm->heap_base - IMAGE_BASE;
    } else if (which == 1) {
        q->kind  = QUERY_PROCMAP_HEAP;
        q->base  = mm->heap_base;
        q->bytes = mm->brk - mm->heap_base;
    } else if (which == 2) {
        q->kind  = QUERY_PROCMAP_STACK;
        q->base  = mm->stack_bottom;
        q->bytes = UADDR_STACK_VADDR + 4096 - mm->stack_bottom;
    } else {
        int seen = 3;
        for (int i = 0; mm->regions && i < mm->region_cap; i++) {
            struct mmap_region *r = &mm->regions[i];
            if (!r->base) continue;
            if (seen++ != which) continue;
            q->kind  = r->kind == MMAP_KIND_FILE ? QUERY_PROCMAP_FILE
                     : r->kind == MMAP_KIND_MMIO ? QUERY_PROCMAP_MMIO
                                                 : QUERY_PROCMAP_ANON;
            q->base  = r->base;
            q->bytes = r->npages * 4096ULL;
            q->prot  = r->prot;
            q->file_off = r->file_off;
            k_strlcpy(q->path, r->path, sizeof q->path);
            return;
        }
    }
}

static int procmap_fill(int index, void *out) {
    if (index < 0) return 0;
    for (int pid = 1; pid <= SYS_PROC_MAX; pid++) {
        struct sched_mm *mm = scheduler_mm_for_pid(pid);
        if (!mm) continue;
        int n = records_of(mm);
        if (index >= n) { index -= n; continue; }
        fill_one(out, pid, mm, index);
        return 1;
    }
    return 0;
}

static const struct query_provider procmap_provider = {
    .cls = QUERY_PROCMAP,
    .name = "procmap",
    .record_size = sizeof(struct query_procmap),
    .flags = QUERY_F_LIST,
    .count = procmap_count,
    .fill = procmap_fill,
    .fields = 0,
    .field_count = 0,
};

void procmap_query_init(void) {
    query_register(&procmap_provider);
}
INITCALL(procmap_query_init, INIT_QUERY);
