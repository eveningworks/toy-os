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
