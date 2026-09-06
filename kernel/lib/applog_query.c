// QUERY_APPLOG -- what the programs said, as a fact ring 3 can read.
//
// The twin of klog_query.c, and deliberately a SEPARATE class rather
// than more records on that one: the two rings are separate so a chatty
// program cannot evict kernel evidence (api/applog.h), and one class
// over both would put them back in a single stream.
//
// `index` COUNTS FROM THE OLDEST RECORD STILL HELD, so index 0 always
// answers and a reader that has fallen behind gets `oldest` back on
// every record to compare against what it wanted.
#include "query.h"
#include "applog.h"
#include "string.h"
#include "initcall.h"

_Static_assert(sizeof(((struct query_applog *)0)->tag) == APPLOG_TAG_MAX &&
               sizeof(((struct query_applog *)0)->text) == APPLOG_TEXT_MAX,
               "the ABI record and the ring must agree -- abi/query_abi.h "
               "cannot include api/applog.h, so the sizes are checked here");

static int applog_count_records(void) {
    uint64_t total = applog_total();
    if (!total) return 0;
    return (int)(total - applog_oldest() + 1);
}

static int applog_fill(int index, void *out) {
    if (index < 0) return 0;
    struct query_applog *q = out;
    struct applog_rec rec;

    uint64_t oldest = applog_oldest();
    if (!applog_get(oldest + (uint64_t)index, &rec)) return 0;

    q->seq = rec.seq;
    q->cs = rec.cs;
    q->total = applog_total();
    // Re-read rather than reusing the value above: a record written
    // between the two calls moves the oldest, and reporting the older
    // one would tell a reader it had lost nothing when it had.
    q->oldest = applog_oldest();
    k_strlcpy(q->tag, rec.tag, sizeof q->tag);
    k_memcpy(q->text, rec.text, rec.len + 1u);
    q->len = rec.len;
    q->eol = rec.eol;
    return 1;
}

static const struct query_provider applog_provider = {
    .cls = QUERY_APPLOG,
    .name = "applog",
    .record_size = sizeof(struct query_applog),
    .flags = QUERY_F_LIST,
    .count = applog_count_records,
    .fill = applog_fill,
    // NO NAMED FIELDS, for klog_query.c's reason: a list is not
    // addressable as one value.
    .fields = NULL,
    .field_count = 0,
};

void applog_query_init(void) {
    query_register(&applog_provider);
}
INITCALL(applog_query_init, INIT_QUERY);
