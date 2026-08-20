// The kernel heap, as queryable FACTS.
//
// TWO CLASSES, and the split is about COST rather than shape.
// QUERY_HEAP reads counters the allocator already maintains and is free
// to ask for. QUERY_HEAPCHECK performs a SCAN of every poisoned free
// block on each read -- which is a fact by this project's definition
// (computed fresh, no stored form) and is emphatically not something a
// caller should get by accident while asking how full the heap is.
//
// A READ THAT DOES WORK is an established shape here, not a new one:
// QUERY_MMAUDIT re-walks every address space per read for the same
// reason. It is what lets `heap check` become a /bin program without
// inventing a write-triggered action -- the alternative would have been
// a tunable whose SET runs a scan, which is how sysctl spells
// drop_caches and is a worse fit for something that returns a result.
#include "query.h"
#include "heap.h"
#include "string.h"
#include <stddef.h>

static int heap_count(void) { return 1; }

static int heap_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_heap *h = out;
    k_memset(h, 0, sizeof *h);
    h->total_bytes       = heap_total_bytes();
    h->used_bytes        = heap_used_bytes();
    // Derived rather than asked for, and NOT total - used: a quarantined
    // block belongs to neither, so subtracting it twice (or not at all)
    // is the arithmetic that would make this column wrong exactly when
    // the debug mode has found something.
    h->quarantined_bytes = heap_quarantined_bytes();
    h->free_bytes        = h->total_bytes - h->used_bytes - h->quarantined_bytes;
    h->rz_checks         = heap_rz_checks();
    h->violations        = heap_violations();
    h->debug             = (uint64_t)heap_debug();
    return 1;
}

static const struct query_field heap_fields[] = {
    QUERY_FIELD(struct query_heap, total_bytes,       QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_heap, used_bytes,        QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_heap, free_bytes,        QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_heap, quarantined_bytes, QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_heap, rz_checks,         QUERY_TYPE_U64),
    QUERY_FIELD(struct query_heap, violations,        QUERY_TYPE_U64),
    QUERY_FIELD(struct query_heap, debug,             QUERY_TYPE_U64),
};

static const struct query_provider heap_provider = {
    .cls = QUERY_HEAP,
    .name = "heap",
    .record_size = sizeof(struct query_heap),
    .flags = 0,
    .count = heap_count,
    .fill = heap_fill,
    .fields = heap_fields,
    .field_count = sizeof heap_fields / sizeof heap_fields[0],
};

static int heapcheck_count(void) { return 1; }

static int heapcheck_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_heapcheck *c = out;
    k_memset(c, 0, sizeof *c);
    // The scan. Before it, so `checked` describes the pass that just
    // ran rather than the one before it.
    uint64_t before = heap_rz_checks();
    c->damaged = heap_check();
    c->checked = heap_rz_checks() - before;
    return 1;
}

// NO NAMED FIELDS, deliberately, even though this is a scalar. A field
// is read by `config get`, and `config get heapcheck.damaged` would run
// a full heap scan as a side effect of what reads like an inspection --
// including from anything that enumerates fields to build a UI. The
// scan is available to a tool that asks for the record on purpose.
static const struct query_provider heapcheck_provider = {
    .cls = QUERY_HEAPCHECK,
    .name = "heapcheck",
    .record_size = sizeof(struct query_heapcheck),
    .flags = 0,
    .count = heapcheck_count,
    .fill = heapcheck_fill,
    .fields = 0,
    .field_count = 0,
};

void heap_query_init(void) {
    query_register(&heap_provider);
    query_register(&heapcheck_provider);
}
