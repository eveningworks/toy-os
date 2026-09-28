// Tests for KASAN (kernel/lib/kasan.c). The check's arithmetic runs in
// every build over a FAKE shadow; the live tests need `make KASAN=1` and
// skip without it. Each live test is a positive control for one class
// of bug, and tools/sanitize_run.py requires their reports to appear.
#include "ktest.h"
#include "kasan.h"
#include "heap.h"
#include "pmm.h"
#include "string.h"

// --- the check, over a fake shadow ---------------------------------------

// Granules starting at address 0x1000: [0] all 8 live, [1] first 3 live,
// [2] a heap redzone, [3] all live.
static const int8_t fake[] = { 0, 3, (int8_t)KASAN_HEAP_REDZONE, 0 };

static size_t scan(uint64_t addr, size_t size) {
    return kasan_scan(fake + ((addr - 0x1000) >> 3), addr, size);
}

KTEST("kasan", "the check finds the first bad byte, granule by granule") {
    KTEST_ASSERT_EQ(scan(0x1000, 8), 0);          // one whole live granule
    KTEST_ASSERT_EQ(scan(0x1004, 4), 0);          // inside it
    KTEST_ASSERT_EQ(scan(0x1008, 3), 0);          // exactly the 3 live bytes
    KTEST_ASSERT_EQ(scan(0x1008, 4), 4);          // one past them: byte 3 is bad
    KTEST_ASSERT_EQ(scan(0x100B, 1), 1);          // starting past them
    KTEST_ASSERT_EQ(scan(0x1004, 8), 8);          // spanning into the partial one
    KTEST_ASSERT_EQ(scan(0x1006, 4), 0);          // across the boundary, still live
    KTEST_ASSERT_EQ(scan(0x1010, 1), 1);          // a redzone
    KTEST_ASSERT_EQ(scan(0x1000, 32), 12);        // a long access stops at the first bad byte
    KTEST_ASSERT_EQ(scan(0x1018, 8), 0);          // live again after it
}

// --- live: one per class of bug ------------------------------------------

#ifdef TOYOS_KASAN
// Each test's site reports once per boot (reports are per instruction),
// so a second run of the suite on the same boot skips rather than fails.
#define ONCE() static int fired; if (fired) KTEST_SKIP("already reported on this boot"); fired = 1
#define LIVE() do { if (!kasan_enabled()) KTEST_SKIP("KASAN is not live on this boot"); } while (0)

static int reported(unsigned before, const char *kind) {
    return kasan_report_count() == before + 1 && k_strstr(kasan_last_report(), kind) != 0;
}

KTEST("kasan", "a write one past a kmalloc block is heap-out-of-bounds") {
    LIVE(); ONCE();
    volatile uint8_t *p = kmalloc(13);
    KTEST_ASSERT(p != 0);
    unsigned before = kasan_report_count();
    p[13] = 1;                        // the first byte past what was asked for
    kfree((void *)p);
    KTEST_ASSERT(reported(before, "heap-out-of-bounds"));
}

KTEST("kasan", "a read through a freed pointer is use-after-free") {
    LIVE(); ONCE();
    volatile uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    kfree((void *)p);
    unsigned before = kasan_report_count();
    (void)p[10];
    KTEST_ASSERT(reported(before, "use-after-free"));
}

KTEST("kasan", "freeing twice is a double-free") {
    LIVE(); ONCE();
    void *p = kmalloc(32);
    KTEST_ASSERT(p != 0);
    kfree(p);
    unsigned before = kasan_report_count();
    kfree(p);
    KTEST_ASSERT(reported(before, "double-free"));
}

KTEST("kasan", "touching a freed frame is a use-after-free of a page") {
    LIVE(); ONCE();
    uint64_t f = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(f != 0);
    pmm_free_frame(f);
    unsigned before = kasan_report_count();
    (void)*(volatile uint32_t *)(uintptr_t)(f + 100);
    KTEST_ASSERT(reported(before, "a freed page"));
}

// noinline + a volatile index, so GCC cannot see the index is out of
// range and warn -- or fold the access away.
__attribute__((noinline)) static uint8_t stack_read(volatile int i) {
    volatile uint8_t buf[16];
    for (int k = 0; k < 16; k++) buf[k] = (uint8_t)k;
    return buf[i];
}

KTEST("kasan", "reading past a stack array is stack-out-of-bounds") {
    LIVE(); ONCE();
    unsigned before = kasan_report_count();
    (void)stack_read(16);
    KTEST_ASSERT(reported(before, "stack-out-of-bounds"));
}

static volatile uint8_t g_kasan_global[13];

KTEST("kasan", "reading past a global array is global-out-of-bounds") {
    LIVE(); ONCE();
    volatile int i = 13;
    unsigned before = kasan_report_count();
    (void)g_kasan_global[i];
    KTEST_ASSERT(reported(before, "global-out-of-bounds"));
}

KTEST("kasan", "a suppressed bad access is not reported") {
    LIVE();
    volatile uint8_t *p = kmalloc(8);
    KTEST_ASSERT(p != 0);
    unsigned before = kasan_report_count();
    kasan_suppress_begin();
    p[8] = 1;
    kasan_suppress_end();
    kfree((void *)p);
    KTEST_ASSERT_EQ(kasan_report_count(), before);
}
#endif
