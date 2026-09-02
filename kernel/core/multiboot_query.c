// The firmware memory map, as a queryable FACT.
//
// Separate from multiboot.c for the same reason mem_query.c is separate
// from pmm.c: the provider is a small adapter over a walk that already
// existed, and keeping it apart leaves multiboot.c about parsing tags.
//
// THE FIRST REAL LIST PROVIDER. `QUERY_PROVIDERS` (class 0) is a list
// too, but it describes the registry rather than any subsystem, so
// until this one the LIST path had exactly one implementation -- which
// this project treats as an unvalidated seam.
//
// WHY IT IS A FACT AT ALL: `meminfo` stayed a kernel builtin long after
// /bin/meminfo existed, because the multiboot map has no provider and
// multiboot_print_meminfo() writes straight to the console. Ring 3
// could report the allocator's totals and not the machine's map.
//
// THE WALK IS RE-RUN PER RECORD, and that is a deliberate trade rather
// than an oversight. multiboot_mmap_foreach() takes a callback with no
// early exit and no index, so answering "region 3" means walking to it;
// caching the map into a static array instead would be a second copy of
// firmware data that must never disagree with the first. The map is a
// handful of entries and this is read by a person running `meminfo`,
// so the walk is free at the scale it happens.
#include "query.h"
#include "multiboot.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"

// multiboot_mmap_foreach() takes a plain callback with no context
// argument, so the in-flight state has to be file-scope. Safe for the
// reason every other static here is: the kernel is single-threaded and
// nothing re-enters a query mid-walk.
static int g_want;              // which region fill() is after, or -1 to count
static int g_seen;              // regions walked so far
static struct query_memmap g_hit;
static int g_found;

static void memmap_cb(const struct multiboot_mmap_region *r) {
    if (g_want >= 0 && g_seen == g_want) {
        g_hit.base = r->base;
        g_hit.length = r->length;
        g_hit.type = r->type;
        g_found = 1;
    }
    g_seen++;
}

static int memmap_walk(int want) {
    g_want = want;
    g_seen = 0;
    g_found = 0;
    multiboot_mmap_foreach(memmap_cb);
    return g_seen;
}

static int memmap_count(void) {
    return memmap_walk(-1); // count only; 0 is a valid answer, not an error
}

static int memmap_fill(int index, void *out) {
    if (index < 0) return 0;
    memmap_walk(index);
    if (!g_found) return 0; // past the end
    k_memcpy(out, &g_hit, sizeof g_hit);
    return 1;
}

// No named fields, and that is the rule for a LIST rather than an
// omission: an index baked into a flat name ("memmap.2.base") means a
// different record a second later. See struct query_provider's `fields`
// comment, and sysctl's worst corner.
static const struct query_provider memmap_provider = {
    .cls = QUERY_MEMMAP,
    .name = "memmap",
    .record_size = sizeof(struct query_memmap),
    .flags = QUERY_F_LIST,
    .count = memmap_count,
    .fill = memmap_fill,
    .fields = 0,
    .field_count = 0,
};

void multiboot_query_init(void) {
    query_register(&memmap_provider);
}
INITCALL(multiboot_query_init, INIT_QUERY);
