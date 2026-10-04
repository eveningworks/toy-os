// What each live process costs in memory -- QUERY_PROCMEM.
//
// A flat list like procpath_query.c, pid in the record. Each record is
// a walk of that process's page tables (vmm_audit_space), done under
// the preemption guard so the owner cannot edit its tables mid-walk on
// this uniprocessor kernel. A walk per process per read: the reason
// this is its own fact rather than a proc_info field, which signal
// delivery and every pid lookup read.
#include "query.h"
#include "scheduler.h"
#include "proc_info.h"
#include "vmm.h"
#include "string.h"
#include "initcall.h"

_Static_assert(sizeof(struct query_procmem) <= QUERY_RECORD_MAX,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");

// The slot is a process with an address space, and not a thread (whose
// space is its leader's, already counted there).
static int is_process(int slot) {
    if (!scheduler_slot_pml4(slot)) return 0;
    struct proc_info pi;
    if (!scheduler_proc_info(slot, &pi) || !pi.pid) return 0;
    return !pi.tgid || pi.tgid == pi.pid;
}

static int procmem_count(void) {
    int n = 0;
    for (int slot = 0; slot < scheduler_slot_end(); slot++) n += is_process(slot);
    return n;
}

static int procmem_fill(int index, void *out) {
    if (index < 0) return 0;
    for (int slot = 0; slot < scheduler_slot_end(); slot++) {
        if (!is_process(slot) || index--) continue;
        struct query_procmem *q = out;
        k_memset(q, 0, sizeof *q);
        q->pid = scheduler_slot_pid(slot);
        struct vmm_audit a;
        scheduler_preempt_disable();
        vmm_audit_space(scheduler_slot_pml4(slot), &a);
        scheduler_preempt_enable();
        q->private_bytes = a.private_bytes;
        q->shared_bytes = a.shared_bytes;
        return 1;
    }
    return 0;
}

static const struct query_provider procmem_provider = {
    .cls = QUERY_PROCMEM,
    .name = "procmem",
    .record_size = sizeof(struct query_procmem),
    .flags = QUERY_F_LIST,
    .count = procmem_count,
    .fill = procmem_fill,
    .fields = 0,
    .field_count = 0,
};

void procmem_query_init(void) {
    query_register(&procmem_provider);
}
INITCALL(procmem_query_init, INIT_QUERY);
