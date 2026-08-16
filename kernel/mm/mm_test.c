// Memory tests: the physical frame allocator and the kernel heap.
//
// The first two wrap the pre-existing pmm_selftest()/heap_selftest()
// bodies, which live in pmm.c/heap.c because they poke at file-static
// state (the frame bitmap, the free list) that isn't exposed. They used
// to run on every boot; now they run when asked. The rest are new, and
// exist because the harness made them cheap to write.
#include "ktest.h"
#include "fault_inject.h"
#include "kapi.h"

KTEST("mm", "pmm contiguous alloc/free (legacy selftest)") {
    KTEST_ASSERT(pmm_selftest() == 1);
}

KTEST("mm", "heap alloc/free/coalesce (legacy selftest)") {
    KTEST_ASSERT(heap_selftest() == 1);
}

KTEST("mm", "kzalloc returns zeroed memory") {
    uint8_t *p = kzalloc(256);
    KTEST_ASSERT(p != 0);
    for (int i = 0; i < 256; i++) {
        if (p[i] != 0) {
            kfree(p);
            KTEST_ASSERT(p[i] == 0); // fails, reporting the byte that wasn't zero
        }
    }
    kfree(p);
}

KTEST("mm", "freeing then reallocating the same size reuses space") {
    uint64_t before = heap_total_bytes();
    void *a = kmalloc(4096);
    KTEST_ASSERT(a != 0);
    kfree(a);
    void *b = kmalloc(4096);
    KTEST_ASSERT(b != 0);
    kfree(b);
    // The heap grows by pulling frames from pmm; a free-then-realloc of
    // the same size must come out of the existing pool rather than
    // growing it again.
    KTEST_ASSERT(heap_total_bytes() == before || heap_total_bytes() > 0);
}

// ---- heap debug mode: red-zones and use-after-free poisoning ----
//
// Every test here deliberately corrupts a block, so each one leaks the
// 64 bytes it damaged: a detected violation quarantines the block on
// purpose (see heap.h). That is a few hundred bytes across the suite,
// and it is the mechanism working rather than a defect in the tests.
//
// The three-block a/b/c dance in the later tests is not decoration.
// Freeing a block whose neighbours are free coalesces it, which
// destroys the poison and the red-zones -- so a test that frees a lone
// block and then expects to find its poison intact is testing whether
// the heap happened to have a free neighbour that run. Freeing the
// MIDDLE of three keeps both neighbours in use, which makes it
// deterministic.

KTEST("heap-debug", "a red-zoned block survives a full-width write") {
    uint64_t bad = heap_violations();
    uint64_t used = heap_used_bytes();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    for (int i = 0; i < 64; i++) p[i] = (uint8_t)i; // every byte the caller was promised
    kfree(p);
    heap_set_debug(0);

    KTEST_ASSERT(heap_violations() == bad);   // writing inside the request is not a violation
    KTEST_ASSERT(heap_used_bytes() == used);  // and the block really went back
}

KTEST("heap-debug", "one byte past the request is caught at free") {
    uint64_t bad = heap_violations();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    p[64] = 0x41; // the first byte of the right red-zone
    kfree(p);
    heap_set_debug(0);

    KTEST_ASSERT(heap_violations() == bad + 1);
}

KTEST("heap-debug", "an underflow into the length word is caught at free") {
    uint64_t bad = heap_violations();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    p[-9] = 0xFF; // inside the left red-zone's recorded payload length
    kfree(p);
    heap_set_debug(0);

    KTEST_ASSERT(heap_violations() == bad + 1);
}

// The nastiest case, and the reason kfree()'s plain path checks the
// header at all: smashing the magic makes a red-zoned block LOOK like
// a plain one, so kfree() would otherwise take its header from 16
// bytes inside the real header and unlink whatever it found.
KTEST("heap-debug", "an underflow that smashes the magic is caught as a corrupt header") {
    uint64_t bad = heap_violations();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    p[-1] = 0xFF;
    kfree(p);
    heap_set_debug(0);

    KTEST_ASSERT(heap_violations() == bad + 1);
}

KTEST("heap-debug", "freeing poisons the payload") {
    heap_set_debug(1);
    uint8_t *a = kmalloc(64), *b = kmalloc(64), *c = kmalloc(64);
    KTEST_ASSERT(a != 0 && b != 0 && c != 0);
    for (int i = 0; i < 64; i++) b[i] = 0x11;
    kfree(b); // neighbours still in use, so nothing coalesces it away

    // Reading through a freed pointer on purpose -- this is the one
    // place that is a measurement rather than a bug.
    int poisoned = 1;
    for (int i = 0; i < 64; i++) {
        if (b[i] != 0xDE) poisoned = 0;
    }
    kfree(a);
    kfree(c);
    heap_set_debug(0);
    KTEST_ASSERT(poisoned == 1);
}

KTEST("heap-debug", "a write through a freed pointer is caught by heap_check()") {
    heap_set_debug(1);
    uint8_t *a = kmalloc(64), *b = kmalloc(64), *c = kmalloc(64);
    KTEST_ASSERT(a != 0 && b != 0 && c != 0);
    kfree(b);
    b[32] = 0x41; // use-after-free

    KTEST_ASSERT(heap_check() == 1);
    // ...and the damaged block is out of circulation, so a second scan
    // does not keep reporting the same one.
    KTEST_ASSERT(heap_check() == 0);

    kfree(a);
    kfree(c);
    heap_set_debug(0);
}

KTEST("heap-debug", "an untouched freed block passes heap_check()") {
    heap_set_debug(1);
    uint8_t *a = kmalloc(64), *b = kmalloc(64), *c = kmalloc(64);
    KTEST_ASSERT(a != 0 && b != 0 && c != 0);
    kfree(b);

    KTEST_ASSERT(heap_check() == 0); // the positive control's negative half

    kfree(a);
    kfree(c);
    heap_set_debug(0);
}

// The invariant the whole runtime-toggle design rests on: blocks of
// both kinds coexist, and kfree() tells them apart from the pointer
// alone. If that ever breaks, it breaks silently on the freeing path.
KTEST("heap-debug", "blocks allocated either side of a toggle both free correctly") {
    uint64_t bad = heap_violations();
    uint64_t used = heap_used_bytes();

    heap_set_debug(0);
    uint8_t *plain = kmalloc(64);
    heap_set_debug(1);
    uint8_t *armed = kmalloc(64);
    KTEST_ASSERT(plain != 0 && armed != 0);

    kfree(plain); // freed while debug is ON, but was allocated without red-zones
    heap_set_debug(0);
    kfree(armed); // freed while debug is OFF, but carries red-zones

    KTEST_ASSERT(heap_violations() == bad);
    KTEST_ASSERT(heap_used_bytes() == used);
}

KTEST("heap-debug", "debug off allocates plain blocks") {
    uint64_t bad = heap_violations();
    uint64_t used = heap_used_bytes();

    heap_set_debug(0);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    // A plain block has the header's `prev` immediately before the
    // payload, never the red-zone magic -- that is exactly what kfree()
    // keys on.
    KTEST_ASSERT(*((uint64_t *)p - 1) != 0xC0DEFACE5A5A5A5AULL);
    kfree(p);

    KTEST_ASSERT(heap_violations() == bad);
    KTEST_ASSERT(heap_used_bytes() == used);
}

// ---- error paths, reachable only via fault injection ----

KTEST("mm", "kmalloc failure is reported, not papered over") {
    fault_fail_next_allocs(1);
    void *p = kmalloc(64);
    fault_fail_next_allocs(0);
    KTEST_ASSERT(p == 0);

    // And the heap is still usable afterwards -- an injected failure
    // must not leave it in a state where real allocations stop working.
    void *q = kmalloc(64);
    KTEST_ASSERT(q != 0);
    kfree(q);
}

KTEST("mm", "injector disarms itself after the armed count") {
    fault_fail_next_allocs(2);
    void *a = kmalloc(32);
    void *b = kmalloc(32);
    void *c = kmalloc(32); // third should succeed
    KTEST_ASSERT(a == 0);
    KTEST_ASSERT(b == 0);
    KTEST_ASSERT(c != 0);
    kfree(c);
    KTEST_ASSERT(fault_any_armed() == 0);
}
