// A general-purpose kernel-space heap: kmalloc()/kfree(), first-fit
// over an address-ordered doubly-linked list of blocks, coalescing
// adjacent free blocks back together on kfree(). See heap.h for the
// public contract and why this exists (multi-instance app windows --
// gui_apps.h's `multi_instance` flag -- were the first thing that
// actually needed dynamic allocation; before this, every app's state
// was a single static struct because there was nothing else to
// allocate it from).
//
// Built directly on pmm.h's physical frame allocator: boot.asm
// identity-maps the entire low 4GiB as kernel/supervisor-only (see
// paging.c's top comment), so a physical frame pmm_alloc_frame()/
// pmm_alloc_contiguous() hands back is ALREADY a valid kernel-space
// pointer -- no separate vmm_map_user_page()-style mapping step needed.
// This file just carves that identity-mapped space into
// variable-sized blocks.
//
// There's no single contiguous region growing via something like
// sbrk()/brk() -- each time the free list can't satisfy a request, a
// fresh region is pulled from pmm (pmm_alloc_contiguous(), NOT
// guaranteed physically adjacent to a previous region) and linked in
// as one new free block. Blocks are kept in a doubly-linked list in
// the order they were added/split, but coalescing on kfree() checks
// REAL address adjacency ((uint8_t*)(b+1) + b->size == (uint8_t*)next),
// not list adjacency -- two blocks next to each other in the list
// might come from separate, non-adjacent pmm regions.
//
// Not interrupt-safe or reentrant, matching this kernel's existing
// single-threaded assumptions elsewhere (see apps/wm/wm.c's top
// comment) -- nothing here is called from an ISR, and the scheduler
// only ever preempts between ring-3 processes, never kernel-mode code
// mid-kmalloc. If a future caller ever needs kmalloc from an interrupt
// handler or a genuinely preemptible kernel thread, this needs a lock
// first.
#include "heap.h"
#include "pmm.h"
#include "klog.h"
#include "string.h"

#define HEAP_ALIGN 16
#define HEAP_PAGE_SIZE 4096
#define HEAP_MIN_GROW_PAGES 16 // 64KiB -- avoids growing one page at a time under a run of small allocations

struct heap_block {
    uint64_t size;            // usable payload size, NOT including this header
    int free;
    struct heap_block *next;  // list order, not necessarily address order across regions -- see top comment
    struct heap_block *prev;
};

static struct heap_block *g_head = 0;
static struct heap_block *g_tail = 0;
static uint64_t g_total_bytes = 0; // sum of every block's payload size ever claimed from pmm, used + free
static uint64_t g_used_bytes = 0;

static uint64_t align_up(uint64_t n, uint64_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

// Pulls `pages` contiguous physical frames from pmm and links them in
// as one new free block at the tail of the list.
static struct heap_block *append_region(uint64_t pages) {
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) return 0;

    struct heap_block *b = (struct heap_block *)phys;
    b->size = pages * HEAP_PAGE_SIZE - sizeof(struct heap_block);
    b->free = 1;
    b->next = 0;
    b->prev = g_tail;
    if (g_tail) g_tail->next = b;
    g_tail = b;
    if (!g_head) g_head = b;

    g_total_bytes += b->size;
    return b;
}

static struct heap_block *grow_heap(uint64_t min_payload) {
    uint64_t needed = min_payload + sizeof(struct heap_block);
    uint64_t pages = (needed + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
    if (pages < HEAP_MIN_GROW_PAGES) pages = HEAP_MIN_GROW_PAGES;
    return append_region(pages);
}

void heap_init(void) {
    g_head = 0;
    g_tail = 0;
    g_total_bytes = 0;
    g_used_bytes = 0;
    // Grows lazily on the first kmalloc() that needs it, rather than
    // reserving anything eagerly here -- a boot that never opens a
    // multi-instance window (or hits any future kmalloc caller)
    // shouldn't cost any memory for this at all.
}

// Shrinks `b` to exactly `size` and, if what's left over is large
// enough to be its own useful block (a header plus at least
// HEAP_ALIGN bytes), carves it off as a new free block right after `b`
// in the list -- keeps a large free region from being handed out whole
// to a small request forever.
static void split_block(struct heap_block *b, uint64_t size) {
    uint64_t remaining = b->size - size;
    if (remaining < sizeof(struct heap_block) + HEAP_ALIGN) return; // not worth splitting

    struct heap_block *rem = (struct heap_block *)((uint8_t *)(b + 1) + size);
    rem->size = remaining - sizeof(struct heap_block);
    rem->free = 1;
    rem->next = b->next;
    rem->prev = b;
    if (b->next) b->next->prev = rem;
    else g_tail = rem;
    b->next = rem;
    b->size = size;
}

void *kmalloc(size_t size) {
    if (size == 0) return 0;
    uint64_t need = align_up(size, HEAP_ALIGN);

    for (struct heap_block *b = g_head; b; b = b->next) {
        if (b->free && b->size >= need) {
            split_block(b, need);
            b->free = 0;
            g_used_bytes += b->size;
            return (void *)(b + 1);
        }
    }

    struct heap_block *grown = grow_heap(need);
    if (!grown) return 0; // out of physical memory
    split_block(grown, need);
    grown->free = 0;
    g_used_bytes += grown->size;
    return (void *)(grown + 1);
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) k_memset(p, 0, size);
    return p;
}

// Merges `b` with its address-adjacent neighbor if that neighbor is
// also free -- checked by real pointer arithmetic, not list adjacency
// (see this file's top comment for why the two aren't the same thing
// here).
static void try_merge_next(struct heap_block *b) {
    struct heap_block *n = b->next;
    if (!n || !n->free) return;
    if ((uint8_t *)(b + 1) + b->size != (uint8_t *)n) return; // not physically adjacent

    b->size += sizeof(struct heap_block) + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
    else g_tail = b;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct heap_block *b = (struct heap_block *)ptr - 1;
    if (b->free) return; // double-free -- silently ignored, same "trust the caller, don't crash" contract as pmm_free_frame()

    g_used_bytes -= b->size;
    b->free = 1;

    try_merge_next(b);            // pull a free right-neighbor into b
    if (b->prev) try_merge_next(b->prev); // pull b (now possibly bigger) into a free left-neighbor
}

uint64_t heap_total_bytes(void) { return g_total_bytes; }
uint64_t heap_used_bytes(void) { return g_used_bytes; }

void heap_selftest(void) {
    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kzalloc(32);
    if (!a || !b || !c) {
        klog_write("heap: selftest FAILED (allocation returned 0)\n");
        return;
    }

    uint8_t *cz = (uint8_t *)c;
    for (int i = 0; i < 32; i++) {
        if (cz[i] != 0) {
            klog_write("heap: selftest FAILED (kzalloc didn't zero)\n");
            return;
        }
    }

    k_memset(a, 0xAA, 64);
    kfree(b); // free the middle block first, then its neighbors -- exercises coalescing on both sides
    kfree(a);
    kfree(c);

    void *d = kmalloc(64);
    if (!d) {
        klog_write("heap: selftest FAILED (alloc after free-and-coalesce)\n");
        return;
    }
    kfree(d);

    klog_write("heap: selftest passed\n");
}
