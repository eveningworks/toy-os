// The process table's limits, as a queryable FACT (QUERY_PROCLIMITS):
// what a ring-3 program sizes a per-process table from, in place of the
// compile-time count the ABI used to carry. `config get proc.max_procs`.
#include "query.h"
#include "scheduler.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"

static int proclimits_count(void) { return 1; }

static int proclimits_fill(int index, void *out) {
    if (index != 0) return 0; // scalar
    struct query_proclimits *q = out;
    k_memset(q, 0, sizeof *q);
    q->max_procs = (uint64_t)scheduler_max_procs();
    q->pid_max = SCHED_PID_MAX;
    q->live = (uint64_t)scheduler_live_count();
    return 1;
}

static const struct query_field proclimits_fields[] = {
    QUERY_FIELD(struct query_proclimits, max_procs, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_proclimits, pid_max,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_proclimits, live,      QUERY_TYPE_U64),
};

static const struct query_provider proclimits_provider = {
    .cls = QUERY_PROCLIMITS,
    .name = "proc",
    .record_size = sizeof(struct query_proclimits),
    .flags = 0, // scalar
    .count = proclimits_count,
    .fill = proclimits_fill,
    .fields = proclimits_fields,
    .field_count = sizeof proclimits_fields / sizeof proclimits_fields[0],
};

void proclimits_query_init(void) {
    query_register(&proclimits_provider);
}
INITCALL(proclimits_query_init, INIT_QUERY);
