// QUERY_REMOTELOG -- what a remote session did, as a fact ring 3 reads.
// The compositor's tray item is the caller; see kernel/include/kernel/
// remote_log.h for why the ring exists at all.
#include "remote_log.h"
#include "query.h"
#include "initcall.h"

static int remotelog_count_records(void) {
    uint64_t total = remote_log_total();
    if (!total) return 0;
    return (int)(total - remote_log_oldest() + 1);
}

static int remotelog_fill(int index, void *out) {
    if (index < 0) return 0;
    // BOUNDED WITHIN THIS CALL, as applog_query.c and klog_query.c are:
    // `oldest` advances as records are written, so a reader walking
    // until the query runs out would never stop on a machine that is
    // logging. The bound is taken once, here.
    uint64_t oldest = remote_log_oldest();
    uint64_t newest = remote_log_total();
    if (!newest || !oldest || (uint64_t)index > newest - oldest) return 0;
    return remote_log_get(oldest + (uint64_t)index, out);
}

static const struct query_provider remotelog_provider = {
    .cls = QUERY_REMOTELOG,
    .name = "remotelog",
    .record_size = sizeof(struct query_remotelog),
    .flags = QUERY_F_LIST,
    .count = remotelog_count_records,
    .fill = remotelog_fill,
    // NO NAMED FIELDS -- a list is not addressable as one value
    // (klog_query.c's reasoning).
    .fields = NULL,
    .field_count = 0,
};

void remote_log_query_init(void) {
    query_register(&remotelog_provider);
}
INITCALL(remote_log_query_init, INIT_QUERY);
