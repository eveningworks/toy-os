// The syscall stall histogram, as a queryable FACT -- `/bin/stalls`.
//
// Only syscalls with a recorded call are listed, so the answer is what
// has actually run rather than every number the table could hold. That
// makes an EMPTY list the honest report when tracking is off, which is a
// different answer from "every syscall took no time" -- the same shape
// and the same reason as QUERY_KSTACK_SYSCALL beside it.
#include "query.h"
#include "syscall_stall.h"
#include "strace.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"

_Static_assert(QUERY_SYSCALL_STALL_BUCKETS ==
                   (int)(sizeof ((struct syscall_stall_info *)0)->bucket /
                         sizeof ((struct syscall_stall_info *)0)->bucket[0]),
               "the ABI record and the kernel's histogram must bucket alike");

static int stall_at(int want, int *out_nr, struct syscall_stall_info *out) {
    int seen = 0;
    for (int nr = 0; nr < SYSCALL_STALL_MAX; nr++) {
        if (!syscall_stall_info(nr, out)) continue;
        if (seen == want) { *out_nr = nr; return 1; }
        seen++;
    }
    return 0;
}

static int stall_count(void) {
    struct syscall_stall_info s;
    int n = 0;
    for (int nr = 0; nr < SYSCALL_STALL_MAX; nr++)
        if (syscall_stall_info(nr, &s)) n++;
    return n;
}

static int stall_fill(int index, void *out) {
    struct syscall_stall_info s;
    int nr;
    if (index < 0 || !stall_at(index, &nr, &s)) return 0;
    struct query_syscall_stall *q = out;
    k_memset(q, 0, sizeof *q);
    q->nr = (uint64_t)nr;
    q->n = s.n;
    q->sum_us = s.sum_us;
    q->max_us = s.max_us;
    for (int i = 0; i < QUERY_SYSCALL_STALL_BUCKETS; i++) q->bucket[i] = s.bucket[i];
    q->tsc_mhz = syscall_stall_mhz();
    // The NAME travels with the number, from strace's table -- the
    // kernel's only list of syscall names. A client mapping numbers
    // itself would grow a second one that drifts.
    const char *nm = strace_syscall_name(nr);
    k_strlcpy(q->name, nm ? nm : "?", sizeof q->name);
    return 1;
}

static const struct query_provider stall_provider = {
    .cls = QUERY_SYSCALL_STALL,
    .name = "stalls",
    .record_size = sizeof(struct query_syscall_stall),
    .flags = QUERY_F_LIST,
    .count = stall_count,
    .fill = stall_fill,
    .fields = 0,
    .field_count = 0,
};

void syscall_stall_query_init(void) {
    query_register(&stall_provider);
}
INITCALL(syscall_stall_query_init, INIT_QUERY);
