// Path-resolution cost, as a queryable fact. See query_abi.h's
// QUERY_FSSTAT for why it is counted rather than argued about.
//
// driver-none: a QUERY provider over tfs3's lookup counters
#include "query.h"
#include "tfs3.h"
#include "string.h"
#include "initcall.h"
#include <stddef.h>

static int fsstat_count(void) { return 1; }

static int fsstat_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_fsstat *r = out;
    k_memset(r, 0, sizeof *r);
    tfs3_lookup_stats(&r->lookup_calls, &r->lookup_reads, &r->lookup_ns);
    return 1;
}

static const struct query_field fsstat_fields[] = {
    QUERY_FIELD(struct query_fsstat, lookup_calls, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_fsstat, lookup_reads, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_fsstat, lookup_ns,    QUERY_TYPE_U64),
};

static const struct query_provider fsstat_provider = {
    .cls = QUERY_FSSTAT,
    .name = "fsstat",
    .record_size = sizeof(struct query_fsstat),
    .flags = 0,
    .count = fsstat_count,
    .fill = fsstat_fill,
    .fields = fsstat_fields,
    .field_count = sizeof fsstat_fields / sizeof fsstat_fields[0],
};

void fs_stat_query_init(void) { query_register(&fsstat_provider); }
INITCALL(fs_stat_query_init, INIT_QUERY);
