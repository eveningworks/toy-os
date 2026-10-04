// Memory, as a queryable FACT.
//
// The provider lives HERE, in the subsystem that owns the numbers,
// rather than in the registry or beside the syscall -- the same rule
// that puts a display_driver in its card's file and a struct setting
// next to the thing it configures. Adding a fact is a struct in abi/, a
// provider, and a registration in whichever subsystem knows the answer;
// there is no central table to edit.
//
// SCALAR: exactly one record, so count() reports 1 and index 0 is the
// only valid index. See docs/settings-and-queries.md's "The vocabulary"
// for what makes this a fact rather than a setting -- nothing here is
// stored, and every field is recomputed on each read.
#include "query.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"
#include "shm.h"
#include "display.h"

static int meminfo_count(void) { return 1; }

static int meminfo_fill(int index, void *out) {
    if (index != 0) return 0; // scalar: nothing else exists to report
    struct query_meminfo *m = out;
    k_memset(m, 0, sizeof *m);
    m->frame_total      = pmm_total_frames();
    m->frame_free       = pmm_free_frames();
    m->frame_bytes      = pmm_frame_size();
    m->heap_total_bytes = heap_total_bytes();
    m->heap_used_bytes  = heap_used_bytes();
    m->phys_usable_bytes = pmm_firmware_bytes();
    m->frame_total_high = pmm_zone_total_frames(PMM_ZONE_ANY);
    m->frame_free_high  = pmm_zone_free_frames(PMM_ZONE_ANY);
    m->frame_reserve_dma32 = pmm_dma32_reserve_frames();
    m->shm_bytes        = shm_total_bytes();
    m->graphics_bytes   = display_graphics_bytes();
    return 1;
}

// The named fields, for `config get mem.frame_free`. Offsets are derived
// by QUERY_FIELD() rather than written out -- a hand-written offset is a
// number somebody has to keep true when the struct changes.
//
// The names are the STRUCT's, deliberately: two names for one number is
// how a doc and a header drift, and there is no reason for the wire name
// to differ from the field a reader will grep for.
static const struct query_field meminfo_fields[] = {
    QUERY_FIELD(struct query_meminfo, frame_total,      QUERY_TYPE_U64),
    QUERY_FIELD(struct query_meminfo, frame_free,       QUERY_TYPE_U64),
    QUERY_FIELD(struct query_meminfo, frame_bytes,      QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_meminfo, heap_total_bytes, QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_meminfo, heap_used_bytes,  QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_meminfo, phys_usable_bytes, QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_meminfo, frame_total_high, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_meminfo, frame_free_high,  QUERY_TYPE_U64),
    QUERY_FIELD(struct query_meminfo, frame_reserve_dma32, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_meminfo, shm_bytes,        QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_meminfo, graphics_bytes,   QUERY_TYPE_BYTES),
};

static const struct query_provider meminfo_provider = {
    .cls = QUERY_MEMINFO,
    .name = "mem",
    .record_size = sizeof(struct query_meminfo),
    .flags = 0, // scalar
    .count = meminfo_count,
    .fill = meminfo_fill,
    .fields = meminfo_fields,
    .field_count = sizeof meminfo_fields / sizeof meminfo_fields[0],
};

void mem_query_init(void) {
    query_register(&meminfo_provider);
}
INITCALL(mem_query_init, INIT_QUERY);
