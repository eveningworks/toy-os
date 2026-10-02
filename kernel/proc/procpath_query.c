// What PROGRAM each live process is running -- QUERY_PROCPATH.
//
// A flat list over every live process, the same shape procmap_query.c
// uses and for the same reason: a list provider's index has nowhere to
// carry a pid, so the pid rides in the record and the reader filters.
//
// Exists so the compositor can ask "what program is this client?"
// without the kernel keeping the answer per WINDOW. See
// docs/winserver-ring3-design.md's stage 6a.
#include "query.h"
#include "scheduler.h"
#include "fs.h"        // FS_PATH_MAX, which the record's width tracks
#include "string.h"
#include "scheduler.h"   // a slot's pid
#include "initcall.h"

_Static_assert(sizeof(struct query_procpath) <= QUERY_RECORD_MAX,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");

static int procpath_count(void) {
    int n = 0;
    for (int slot = 0; slot < SCHED_MAX_PROCS; slot++)
        if (scheduler_mm_for_pid(scheduler_slot_pid(slot))) n++;
    return n;
}

static int procpath_fill(int index, void *out) {
    if (index < 0) return 0;
    for (int slot = 0; slot < SCHED_MAX_PROCS; slot++) {
        int pid = scheduler_slot_pid(slot);
        if (!scheduler_mm_for_pid(pid)) continue;
        if (index--) continue;
        struct query_procpath *q = out;
        k_memset(q, 0, sizeof *q);
        q->pid = pid;
        // A process with no path answers with "" rather than failing:
        // the record exists, the identity does not.
        if (!scheduler_exec_path(pid, q->path, sizeof q->path)) q->path[0] = '\0';
        return 1;
    }
    return 0;
}

static const struct query_provider procpath_provider = {
    .cls = QUERY_PROCPATH,
    .name = "procpath",
    .record_size = sizeof(struct query_procpath),
    .flags = QUERY_F_LIST,
    .count = procpath_count,
    .fill = procpath_fill,
    .fields = 0,
    .field_count = 0,
};

void procpath_query_init(void) {
    query_register(&procpath_provider);
}
INITCALL(procpath_query_init, INIT_QUERY);
