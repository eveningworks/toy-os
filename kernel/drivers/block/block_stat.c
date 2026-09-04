// The block layer's per-operation counters, and the QUERY class over
// them. See block_stat.h for what they are for.
//
// driver-none: counters and a QUERY provider over block.c's own state
#include "block_stat.h"
#include "query.h"
#include "string.h"
#include "initcall.h"
#include <stddef.h>

static struct {
    uint64_t calls;
    uint64_t sectors;
    uint64_t ns;
    uint64_t failures;
} g_stat[BLK_STAT_OPS];

static const char *const OP_NAME[BLK_STAT_OPS] = {
    "read", "write", "flush", "trim",
};

const char *blk_stat_op_name(int op) {
    if (op < 0 || op >= BLK_STAT_OPS) return "?";
    return OP_NAME[op];
}

void blk_stat_add(int op, uint32_t sectors, uint64_t ns, int ok) {
    if (op < 0 || op >= BLK_STAT_OPS) return;
    g_stat[op].calls++;
    g_stat[op].sectors += sectors;
    g_stat[op].ns += ns;
    if (!ok) g_stat[op].failures++;
}

void blk_stat_reset(void) {
    k_memset(g_stat, 0, sizeof g_stat);
}

int blk_stat_get(int op, uint64_t *calls, uint64_t *sectors,
                 uint64_t *ns, uint64_t *failures) {
    if (op < 0 || op >= BLK_STAT_OPS) return 0;
    if (calls)    *calls    = g_stat[op].calls;
    if (sectors)  *sectors  = g_stat[op].sectors;
    if (ns)       *ns       = g_stat[op].ns;
    if (failures) *failures = g_stat[op].failures;
    return 1;
}

// ---- the QUERY provider ---------------------------------------------

// FIXED AT BLK_STAT_OPS, not "however many have been used". An op with
// no calls yet is a fact worth reporting -- "nothing flushed" and "this
// kernel cannot flush" are different answers, and a list that omits the
// row cannot give the first one.
static int blkstat_count(void) { return BLK_STAT_OPS; }

static int blkstat_fill(int index, void *out) {
    if (index < 0 || index >= BLK_STAT_OPS) return 0;
    struct query_blkstat *r = out;
    k_memset(r, 0, sizeof *r);
    k_strlcpy(r->name, OP_NAME[index], sizeof r->name);
    blk_stat_get(index, &r->calls, &r->sectors, &r->ns, &r->failures);
    return 1;
}

static const struct query_field blkstat_fields[] = {
    QUERY_FIELD(struct query_blkstat, calls,    QUERY_TYPE_U64),
    QUERY_FIELD(struct query_blkstat, sectors,  QUERY_TYPE_U64),
    QUERY_FIELD(struct query_blkstat, ns,       QUERY_TYPE_U64),
    QUERY_FIELD(struct query_blkstat, failures, QUERY_TYPE_U64),
};

static const struct query_provider blkstat_provider = {
    .cls = QUERY_BLKSTAT,
    .name = "blkstat",
    .record_size = sizeof(struct query_blkstat),
    .flags = QUERY_F_LIST,
    .count = blkstat_count,
    .fill = blkstat_fill,
    .fields = blkstat_fields,
    .field_count = sizeof blkstat_fields / sizeof blkstat_fields[0],
};

void block_stat_query_init(void) {
    query_register(&blkstat_provider);
}
INITCALL(block_stat_query_init, INIT_QUERY);
