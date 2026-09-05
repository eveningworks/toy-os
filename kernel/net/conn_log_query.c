// The connection log as queryable facts -- what `/bin/netlog` reads.
// A LIST, oldest record first, the same shape as net_query.c.
#include "query.h"
#include "conn_log.h"
#include "initcall.h"

// driver-none: a QUERY provider over the connection log

static int connlog_count(void) { return conn_log_count(); }

static int connlog_fill(int index, void *out) {
    return conn_log_at(index, out);
}

static const struct query_provider connlog_provider = {
    .cls = QUERY_CONNLOG,
    .name = "connlog",
    .record_size = sizeof(struct query_connlog),
    .flags = QUERY_F_LIST,
    .count = connlog_count,
    .fill = connlog_fill,
    .fields = 0,
    .field_count = 0,
};

void conn_log_query_init(void) {
    query_register(&connlog_provider);
}
INITCALL(conn_log_query_init, INIT_QUERY);
