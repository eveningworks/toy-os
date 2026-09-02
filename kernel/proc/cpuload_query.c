// How the machine's time was spent, as a queryable FACT.
//
// Beside the scheduler because the scheduler is what knows: the split
// comes straight out of bill_current(), so this cannot drift from what
// `ps` and Task Manager report per process.
//
// WHY A FACT AND NOT A PERCENTAGE. The counters are cumulative since
// boot, and a caller divides two samples' deltas. A kernel that reported
// a percentage would have to choose the window, and every caller wants a
// different one -- Task Manager refreshes twice a second, a status line
// might want ten seconds. Linux's /proc/stat makes the same call.
#include "query.h"
#include "scheduler.h"
#include "clocksource.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"

static int cpuload_count(void) { return 1; }

static int cpuload_fill(int index, void *out) {
    if (index != 0) return 0; // scalar
    struct query_cpuload *c = out;
    k_memset(c, 0, sizeof *c);
    scheduler_cpu_time(&c->proc_ns, &c->kernel_ns);
    c->now_ns = clocksource_now_ns();
    return 1;
}

static const struct query_field cpuload_fields[] = {
    QUERY_FIELD(struct query_cpuload, proc_ns,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cpuload, kernel_ns, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cpuload, now_ns,    QUERY_TYPE_U64),
};

static const struct query_provider cpuload_provider = {
    .cls = QUERY_CPULOAD,
    .name = "cpu",
    .record_size = sizeof(struct query_cpuload),
    .flags = 0, // scalar
    .count = cpuload_count,
    .fill = cpuload_fill,
    .fields = cpuload_fields,
    .field_count = sizeof cpuload_fields / sizeof cpuload_fields[0],
};

void cpuload_query_init(void) {
    query_register(&cpuload_provider);
}
INITCALL(cpuload_query_init, INIT_QUERY);
