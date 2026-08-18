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
// single-threaded assumptions elsewhere (see userland/wm/wm.c's top
// comment) -- nothing here is called from an ISR, and the scheduler
// only ever preempts between ring-3 processes, never kernel-mode code
// mid-kmalloc. If a future caller ever needs kmalloc from an interrupt
// handler or a genuinely preemptible kernel thread, this needs a lock
// first.
//
// DEBUG MODE (`heap debug on`, heap_set_debug()) wraps every subsequent
// allocation in red-zones and poisons what it frees:
//
//   off: [header][............ payload ............]
//   on:  [header][span][MAGIC][ payload ][MAGIC][MAGIC]
//                            ^-- the pointer the caller gets
//
// so an overflow past the request, an underflow just before it, and a
// write through an already-freed pointer each land in a known byte
// pattern that kfree() (or the next kmalloc() to reuse the block)
// checks. The mode is a RUNTIME switch rather than a build flag so it
// is reachable in a booted OS and so one build covers both states --
// which means blocks allocated before and after a toggle coexist, and
// kfree() has to tell them apart from the pointer alone.
//
// The trap that makes that sound: kfree() decides by reading the eight
// bytes immediately before the payload, which are HEAP_RZ_MAGIC in a
// red-zoned block and the header's `prev` pointer in a plain one. That
// can never be ambiguous because every heap pointer is an address in
// the identity-mapped low 4GiB and so fits in 32 bits, while
// HEAP_RZ_MAGIC's top half is nonzero. Change either fact -- a heap
// above 4GiB, or a magic that fits in 32 bits -- and the two cases
// become indistinguishable, silently, on the freeing path.
#include "heap.h"
#include "heap_os.h"  // the three things this file deliberately does not know
#include "kfmt.h"     // k_snprintf -- freestanding half, see kfmt.h
#include "string.h"

#define HEAP_ALIGN 16
#define HEAP_PAGE_SIZE 4096
#define HEAP_MIN_GROW_PAGES 16 // 64KiB -- avoids growing one page at a time under a run of small allocations

// Per side, and a multiple of HEAP_ALIGN so the payload stays 16-byte
// aligned with the red-zones in front of it.
#define HEAP_RZ_SIZE 16
// Top half nonzero on purpose -- see this file's top comment.
#define HEAP_RZ_MAGIC 0xC0DEFACE5A5A5A5AULL
#define HEAP_POISON 0xDE

// `free` is tri-state rather than a bool: a block freed while debug
// mode was on still carries intact red-zones and a poisoned payload,
// and the next kmalloc() to reuse it verifies both before handing it
// out. That check is the only thing that can catch a write through an
// already-freed pointer.
#define HEAP_IN_USE 0
#define HEAP_FREE 1
#define HEAP_FREE_POISONED 2

// Sits in the four bytes of padding the compiler was inserting after
// `free` anyway, so it costs nothing, and it is what makes kfree()'s
// plain path able to reject a pointer that isn't one of ours.
#define HEAP_HDR_MAGIC 0x48454150u // 'HEAP'

struct heap_block {
    uint64_t size;            // usable payload size, NOT including this header
    int free;
    uint32_t magic;           // HEAP_HDR_MAGIC -- see header_plausible()
    struct heap_block *next;  // list order, not necessarily address order across regions -- see top comment
    struct heap_block *prev;  // MUST stay the last field: kfree() reads these 8 bytes to tell a red-zoned block from a plain one
};

static struct heap_block *g_head = 0;
static struct heap_block *g_tail = 0;
static uint64_t g_total_bytes = 0; // sum of every block's payload size ever claimed from pmm, used + free
static uint64_t g_used_bytes = 0;

static int g_debug = 0;
static uint64_t g_rz_checks = 0;
static uint64_t g_rz_violations = 0;
static uint64_t g_quarantined_bytes = 0;

static uint64_t align_up(uint64_t n, uint64_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

// Asks the platform for one contiguous run of `pages` and links it in
// as a new free block at the tail of the list.
//
// heap_os_alloc() is the ONLY way this file obtains memory, which is
// what lets the same allocator serve two rings: the kernel hands back
// identity-mapped physical frames, ring 3 hands back sbrk'd address
// space. Neither is visible from here.
static struct heap_block *append_region(uint64_t pages) {
    void *region = heap_os_alloc(pages * HEAP_PAGE_SIZE);
    if (!region) return 0;

    struct heap_block *b = (struct heap_block *)region;
    b->size = pages * HEAP_PAGE_SIZE - sizeof(struct heap_block);
    b->free = HEAP_FREE;
    b->magic = HEAP_HDR_MAGIC;
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
    rem->free = HEAP_FREE;
    rem->magic = HEAP_HDR_MAGIC;
    rem->next = b->next;
    rem->prev = b;
    if (b->next) b->next->prev = rem;
    else g_tail = rem;
    b->next = rem;
    b->size = size;
}

// ---- red-zones and poisoning (debug mode only) ----

// Reports a violation and returns 0, so every caller can end with
// `return rz_fail(...)` and quarantining stays one decision.
static int rz_fail(const char *what, const struct heap_block *b,
                   uint64_t expected, uint64_t found) {
    g_rz_violations++;
    g_quarantined_bytes += b->size;
    // %lx/%lu, not %x/%u: kfmt reads a 32-bit argument without the `l`
    // (see kfmt.c), which silently truncates every value here.
    // Formatted here and handed to the platform as text, rather than
    // calling a logger directly: the kernel's sink is klog and ring
    // 3's is stderr, and this file must not know which it is talking
    // to. k_snprintf is the freestanding half of kfmt (see kfmt.h), so
    // it is available on both sides.
    char msg[192];
    k_snprintf(msg, sizeof msg,
               "heap: RED-ZONE VIOLATION %s block=0x%lx size=%lu\n",
               what, (uint64_t)(uintptr_t)b, b->size);
    heap_os_report(msg);
    k_snprintf(msg, sizeof msg,
               "heap:   expected 0x%lx, found 0x%lx -- block quarantined, not returned to the free list\n",
               expected, found);
    heap_os_report(msg);
    return 0;
}

// Lays out [span][MAGIC] before the payload and [MAGIC][MAGIC] after
// it, and returns the pointer the caller gets. `span` is the payload
// the caller may legitimately touch; b->size can exceed span + 2 *
// HEAP_RZ_SIZE when split_block() declined to carve off the remainder,
// and that slack sits AFTER the right red-zone rather than inside it.
static void *rz_arm(struct heap_block *b, uint64_t span) {
    uint64_t *lead = (uint64_t *)(b + 1);
    lead[0] = span;
    lead[1] = HEAP_RZ_MAGIC;

    uint8_t *payload = (uint8_t *)(lead + 2);
    uint64_t *trail = (uint64_t *)(payload + span);
    trail[0] = HEAP_RZ_MAGIC;
    trail[1] = HEAP_RZ_MAGIC;
    return payload;
}

// Is `ptr` the payload of a red-zoned block? See the top comment for
// why reading the eight bytes before it cannot be ambiguous.
static int rz_armed(const void *ptr) {
    return *((const uint64_t *)ptr - 1) == HEAP_RZ_MAGIC;
}

// Checks both red-zones of an armed block. `span_out` receives the
// recorded payload length. Returns 0 (and has already reported) on a
// violation.
static int rz_check(struct heap_block *b, const void *ptr, uint64_t *span_out) {
    g_rz_checks++;

    const uint64_t *lead = (const uint64_t *)(b + 1);
    uint64_t span = lead[0];
    // The length word is itself inside the left red-zone, so an
    // underflow of 9..16 bytes lands here rather than on the magic.
    // Rejecting an impossible span is what turns that into a report
    // instead of a wild pointer.
    if (span == 0 || (span % HEAP_ALIGN) != 0 || span + 2 * HEAP_RZ_SIZE > b->size)
        return rz_fail("left red-zone (length word)", b, b->size, span);
    if (lead[1] != HEAP_RZ_MAGIC)
        return rz_fail("left red-zone", b, HEAP_RZ_MAGIC, lead[1]);

    const uint64_t *trail = (const uint64_t *)((const uint8_t *)ptr + span);
    if (trail[0] != HEAP_RZ_MAGIC)
        return rz_fail("right red-zone", b, HEAP_RZ_MAGIC, trail[0]);
    if (trail[1] != HEAP_RZ_MAGIC)
        return rz_fail("right red-zone (second word)", b, HEAP_RZ_MAGIC, trail[1]);

    *span_out = span;
    return 1;
}

// Verifies a poisoned free block before it is handed out again: the
// red-zones must still stand AND every payload byte must still be
// HEAP_POISON. Anything else is a write through a pointer whose owner
// already freed it.
static int rz_check_poison(struct heap_block *b) {
    void *ptr = (uint8_t *)(b + 1) + HEAP_RZ_SIZE;
    uint64_t span = 0;
    if (!rz_check(b, ptr, &span)) return 0;

    const uint8_t *p = (const uint8_t *)ptr;
    for (uint64_t i = 0; i < span; i++) {
        if (p[i] != HEAP_POISON)
            return rz_fail("use-after-free (poison overwritten)", b, HEAP_POISON, p[i]);
    }
    return 1;
}

// Takes a block permanently out of circulation. Leaking it is the
// point: its metadata is the thing that proved untrustworthy, so
// putting it back on the free list hands the damage to the next
// allocation.
static void quarantine(struct heap_block *b) {
    b->free = HEAP_IN_USE;
}

// Could this plausibly be a header this allocator wrote? Checked on
// kfree()'s plain path, and it is not paranoia: an underflow of 1..8
// bytes lands on the magic, which makes an armed block look plain, and
// kfree() would then take its "header" from 16 bytes inside the real
// one and start unlinking whatever it found there. This turns that
// into a report. Deliberately cheap and always on, debug mode or not.
static int header_plausible(const struct heap_block *b) {
    if (b->magic != HEAP_HDR_MAGIC) return 0;
    if (b->free != HEAP_IN_USE && b->free != HEAP_FREE && b->free != HEAP_FREE_POISONED) return 0;
    if (b->size == 0 || b->size > g_total_bytes) return 0;
    return 1;
}

void *kmalloc(size_t size) {
    // Inert unless a test armed it (the kernel wires this to
    // fault_inject.h; ring 3 answers 0 always). Returning NULL here is
    // exactly what a genuinely exhausted heap does, which is the whole
    // point -- it exercises every caller's failure path.
    if (heap_os_should_fail_alloc()) return 0;
    if (size == 0) return 0;
    uint64_t span = align_up(size, HEAP_ALIGN);
    uint64_t rz = g_debug ? HEAP_RZ_SIZE : 0;
    uint64_t need = span + 2 * rz; // a red-zone on each side of the payload

    for (struct heap_block *b = g_head; b; b = b->next) {
        if (b->free == HEAP_IN_USE || b->size < need) continue;
        // A poisoned block is verified BEFORE it is split or handed
        // out -- once it is reused, the evidence is gone.
        if (b->free == HEAP_FREE_POISONED && !rz_check_poison(b)) {
            quarantine(b);
            continue; // damaged; keep looking rather than handing it out
        }
        split_block(b, need);
        b->free = HEAP_IN_USE;
        g_used_bytes += b->size;
        return rz ? rz_arm(b, span) : (void *)(b + 1);
    }

    struct heap_block *grown = grow_heap(need);
    if (!grown) return 0; // out of physical memory
    split_block(grown, need);
    grown->free = HEAP_IN_USE;
    g_used_bytes += grown->size;
    return rz ? rz_arm(grown, span) : (void *)(grown + 1);
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
    if (!n || n->free == HEAP_IN_USE) return;
    if ((uint8_t *)(b + 1) + b->size != (uint8_t *)n) return; // not physically adjacent

    b->size += sizeof(struct heap_block) + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
    else g_tail = b;
    // The merged region is no longer one verifiable span -- n's header
    // now sits inside b's payload where poison used to be -- so it
    // stops claiming to be checkable. Losing that check on merge is
    // the deliberate cost of coalescing; the red-zone check at free
    // time has already run by this point.
    b->free = HEAP_FREE;
}

void kfree(void *ptr) {
    if (!ptr) return;

    struct heap_block *b;
    uint64_t span = 0;
    int armed = rz_armed(ptr);
    if (armed) {
        b = (struct heap_block *)((uint8_t *)ptr - HEAP_RZ_SIZE) - 1;
        if (b->free != HEAP_IN_USE) return; // double-free, same contract as below
        if (!rz_check(b, ptr, &span)) {
            // Accounting stays as-is: a quarantined block is still
            // held, it just can never be handed out again.
            quarantine(b);
            return;
        }
    } else {
        b = (struct heap_block *)ptr - 1;
        if (!header_plausible(b)) {
            g_rz_violations++;
            char msg[160];
            k_snprintf(msg, sizeof msg,
                       "heap: CORRUPT HEADER at 0x%lx (size=%lu state=%lu) -- refusing to free\n",
                       (uint64_t)(uintptr_t)b, b->size, (uint64_t)b->free);
            heap_os_report(msg);
            return;
        }
    }
    if (b->free != HEAP_IN_USE) return; // double-free -- silently ignored, same "trust the caller, don't crash" contract as pmm_free_frame()

    g_used_bytes -= b->size;
    if (armed) {
        k_memset(ptr, HEAP_POISON, span);
        b->free = HEAP_FREE_POISONED;
    } else {
        b->free = HEAP_FREE;
    }

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
    // read heap_used_bytes() for real (Task Manager, see the git history)
    // and displayed an impossible ~16 exabyte figure -- caused by a
    // LATER kfree() of that same corrupted block subtracting its
    // inflated size from g_used_bytes, underflowing the unsigned
    // counter. See docs/decisions.md for the full story.
    if (b->prev && b->prev->free != HEAP_IN_USE) try_merge_next(b->prev);
}

uint64_t heap_total_bytes(void) { return g_total_bytes; }
uint64_t heap_used_bytes(void) { return g_used_bytes; }

// Walks every poisoned free block and verifies it now, rather than
// waiting for the allocation that happens to reuse it. Without this, a
// use-after-free is only ever caught at reuse -- which may be a
// thousand allocations later, in an unrelated subsystem, or never.
// Returns the number of violations found; each one is reported and its
// block quarantined, exactly as at reuse.
uint64_t heap_check(void) {
    uint64_t before = g_rz_violations;
    for (struct heap_block *b = g_head; b; b = b->next) {
        if (b->free != HEAP_FREE_POISONED) continue;
        if (!rz_check_poison(b)) quarantine(b);
    }
    return g_rz_violations - before;
}

// Affects allocations made from here on, not existing ones -- see the
// top comment on why both kinds have to coexist.
void heap_set_debug(int on) { g_debug = on ? 1 : 0; }
int heap_debug(void) { return g_debug; }
uint64_t heap_rz_checks(void) { return g_rz_checks; }
uint64_t heap_violations(void) { return g_rz_violations; }
uint64_t heap_quarantined_bytes(void) { return g_quarantined_bytes; }

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
        heap_os_report("heap: selftest FAILED (allocation returned 0)\n");
        return 0; // failure -- see the message above
    }

    uint8_t *cz = (uint8_t *)c;
    for (int i = 0; i < 32; i++) {
        if (cz[i] != 0) {
            heap_os_report("heap: selftest FAILED (kzalloc didn't zero)\n");
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
        heap_os_report("heap: selftest FAILED (used_bytes not back to its starting level after freeing everything)\n");
        return 0; // failure -- see the message above
    }

    void *d = kmalloc(64);
    if (!d) {
        heap_os_report("heap: selftest FAILED (alloc after free-and-coalesce)\n");
        return 0; // failure -- see the message above
    }
    kfree(d);

    if (heap_used_bytes() != used_before) {
        heap_os_report("heap: selftest FAILED (used_bytes not back to its starting level after final free)\n");
        return 0; // failure -- see the message above
    }

    heap_os_report("heap: selftest passed\n");
    return 1;
}
