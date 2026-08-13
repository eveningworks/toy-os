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
#include "fault_inject.h"
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
    // See fault_inject.h -- inert unless a test armed it. Returning
    // NULL here is exactly what a genuinely exhausted heap does.
    if (fault_should_fail_alloc()) return 0;
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
    // Pull b (now possibly bigger) into a free left-neighbor -- but
    // ONLY if that left-neighbor is itself free. try_merge_next()
    // only checks whether the *next* pointer's target is free before
    // merging; it doesn't check whether the block passed in (here,
    // b->prev) is free. Without this guard, freeing a block whose
    // list-previous neighbor is still in USE would silently fold this
    // freed block's size into that in-use block's `size` field --
    // corrupting its accounting (it would report itself as bigger than
    // it was ever allocated for) without ever adding the extra bytes
    // to g_used_bytes. The bug stayed invisible until something first
    // read heap_used_bytes() for real (Task Manager, see CHANGELOG.md)
    // and displayed an impossible ~16 exabyte figure -- caused by a
    // LATER kfree() of that same corrupted block subtracting its
    // inflated size from g_used_bytes, underflowing the unsigned
    // counter. See docs/decisions.md for the full story.
    if (b->prev && b->prev->free) try_merge_next(b->prev);
}

uint64_t heap_total_bytes(void) { return g_total_bytes; }
uint64_t heap_used_bytes(void) { return g_used_bytes; }

int heap_selftest(void) {
    // Baseline rather than an absolute 0. This used to check
    // heap_used_bytes() == 0 after freeing everything it allocated,
    // which was true only because it ran from kernel_main() immediately
    // after heap_init(), before anything else existed. Run from `ktest`
    // in a booted system -- shell, filesystem buffers, GUI state all
    // holding allocations -- that check fails on a perfectly healthy
    // heap. Comparing against the level on entry keeps exactly the
    // property this test exists for (see the comment below: a
    // coalescing bug that corrupts a neighbour shows up as an
    // accounting mismatch) without assuming it owns the machine.
    uint64_t used_before = heap_used_bytes();

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kzalloc(32);
    if (!a || !b || !c) {
        klog_write("heap: selftest FAILED (allocation returned 0)\n");
        return 0; // failure -- see the message above
    }

    uint8_t *cz = (uint8_t *)c;
    for (int i = 0; i < 32; i++) {
        if (cz[i] != 0) {
            klog_write("heap: selftest FAILED (kzalloc didn't zero)\n");
            return 0; // failure -- see the message above
        }
    }

    k_memset(a, 0xAA, 64);
    kfree(b); // free the middle block first, then its neighbors -- exercises coalescing on both sides
    kfree(a);
    kfree(c);

    // Every block from a/b/c is free again at this point -- g_used_bytes
    // should be back to whatever it was before this test allocated
    // anything. This
    // check exists specifically because a real bug once slipped past
    // this self-test entirely: freeing a's still-in-use LEFT neighbor
    // was silently folded into it by the coalescing logic, corrupting
    // that neighbor's size without it ever showing up as a crash or a
    // failed allocation here -- only a later kfree() of the corrupted
    // block would underflow g_used_bytes, and nothing here used to
    // check g_used_bytes at all. See docs/decisions.md.
    if (heap_used_bytes() != used_before) {
        klog_write("heap: selftest FAILED (used_bytes not back to its starting level after freeing everything)\n");
        return 0; // failure -- see the message above
    }

    void *d = kmalloc(64);
    if (!d) {
        klog_write("heap: selftest FAILED (alloc after free-and-coalesce)\n");
        return 0; // failure -- see the message above
    }
    kfree(d);

    if (heap_used_bytes() != used_before) {
        klog_write("heap: selftest FAILED (used_bytes not back to its starting level after final free)\n");
        return 0; // failure -- see the message above
    }

    klog_write("heap: selftest passed\n");
    return 1;
}
