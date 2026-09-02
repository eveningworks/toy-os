// QUERY_KLOG -- the kernel log, as a fact ring 3 can read.
//
// `dmesg` was a ring-0 shell builtin because the log had no way out of
// the kernel: klog_dump() streams into a callback, which is the wrong
// shape for a syscall that has to fill a buffer and return. So the
// command could not follow `ps`, `meminfo` and the rest into /bin, and
// a Terminal window -- where a person actually reads a log -- could not
// run it at all.
//
// It is a provider rather than a syscall for the reason api/query.h
// opens with: a syscall per information class grows the syscall number
// by one per fact and leaves nothing able to answer "what facts exist?".
// docs/roadmap.md's item is specifically "move the introspection
// commands onto it".
#include "query.h"
#include "klog.h"
#include "initcall.h"

static int klog_count_records(void) {
    uint32_t retained = klog_retained_bytes();
    if (!retained) return 0;
    // Ceiling division: a partial last slice is a record like any
    // other, and rounding it away would silently drop the most recent
    // bytes -- which are the ones somebody running dmesg wants most.
    return (int)((retained + QUERY_KLOG_DATA - 1) / QUERY_KLOG_DATA);
}

static int klog_fill(int index, void *out) {
    if (index < 0) return 0;

    struct query_klog *q = out;
    uint64_t total = klog_total_bytes();
    uint32_t retained = klog_retained_bytes();
    uint64_t oldest = total - retained;

    uint64_t want = oldest + (uint64_t)index * QUERY_KLOG_DATA;
    if (want >= total) return 0;

    q->total = total;
    q->retained = retained;
    // klog_read() clamps `want` up to whatever is still retained and
    // reports where it really started, so `first` is the truth rather
    // than what was asked for. A reader that finds it ahead of where
    // the previous record ended has lost bytes to the ring moving.
    q->len = klog_read(want, (char *)q->data, QUERY_KLOG_DATA, &q->first);
    return 1;
}

static const struct query_provider klog_provider = {
    .cls = QUERY_KLOG,
    .name = "klog",
    .record_size = sizeof(struct query_klog),
    .flags = QUERY_F_LIST,
    .count = klog_count_records,
    .fill = klog_fill,
    // NO NAMED FIELDS: a list is not addressable as one value, and
    // `klog.len` would name a different slice on every read.
    .fields = NULL,
    .field_count = 0,
};

void klog_query_init(void) {
    query_register(&klog_provider);
}
INITCALL(klog_query_init, INIT_QUERY);
