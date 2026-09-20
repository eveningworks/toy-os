#ifndef KTEST_H
#define KTEST_H

// In-kernel test harness. A test is a function that runs inside the
// booted kernel with full access to whatever it's testing -- the heap,
// the page allocator, the real filesystem -- which is the whole point:
// these are the checks that can't be done on the host, because they're
// about how this kernel behaves on this hardware.
//
// Registration is automatic. KTEST() emits a descriptor into a
// dedicated `.ktests` linker section (see linker.ld), and the runner
// walks that section from __ktests_start to __ktests_end. There is no
// registry to add a line to and no Makefile edit -- a new test file
// under kernel/ is compiled by the recursive source discovery and its
// tests appear in the next `ktest` run. Same mechanism Linux uses for
// initcalls and KUnit, for the same reason: a list you have to maintain
// by hand is a list that drifts.
//
// What replaced what: this kernel used to call pmm_selftest(),
// heap_selftest() and tfs_selftest() directly from kernel_main(), on
// every single boot. They were real tests that caught
// real bugs, but they ran whether you wanted them or not (the
// filesystem one wrote 64 bytes at a 4.6GB offset every disk-backed
// boot), couldn't be run individually, and a failure printed a line and
// carried on booting -- nothing failed, nothing exited non-zero, and CI
// sailed straight past it.

#include <stdint.h>
#include "ktest_run.h" // ktest_run_all() -- app-facing, so it lives in include/api/

struct ktest_ctx;

// aligned(32) is load-bearing, not decoration. The three pointers are
// 24 bytes with natural alignment 8, and the linker aligns each object
// file's .ktests contribution independently -- which inserted 8 bytes of
// padding and made the section 440 bytes for 18 entries. The runner then
// walked it as an array of 24-byte structs and read the padding as an
// entry (and, because pointer subtraction on a non-multiple of the
// element size is undefined behaviour, GCC's divide-by-24 reciprocal
// turned the count into 0xAAAAAAAD -- a garbage suite name, then a
// kernel panic).
//
// Forcing 32-byte alignment makes sizeof 32 as well, so every entry and
// every inter-object gap is a whole number of entries. The 8 wasted
// bytes per test are worth not having a layout that works until the day
// a new test file shifts the padding.
struct ktest_case {
    const char *suite;
    const char *name;
    void (*fn)(struct ktest_ctx *ctx);
} __attribute__((aligned(32)));

// Defines a test. Use it as a function body:
//
//     KTEST("heap", "coalesces adjacent frees") {
//         void *a = kmalloc(64);
//         kfree(a);
//         KTEST_ASSERT(heap_free_bytes() >= 64);
//     }
//
// `suite` groups tests in the report and is what `ktest <suite>` filters
// on. Keep it to the subsystem name.
#define KTEST_CONCAT_(a, b) a##b
#define KTEST_CONCAT(a, b) KTEST_CONCAT_(a, b)

#define KTEST(suite_str, name_str)                                            \
    static void KTEST_CONCAT(ktest_fn_, __LINE__)(struct ktest_ctx *ctx);     \
    static const struct ktest_case KTEST_CONCAT(ktest_case_, __LINE__)        \
        __attribute__((used, section(".ktests"))) = {                         \
            .suite = suite_str,                                               \
            .name = name_str,                                                 \
            .fn = KTEST_CONCAT(ktest_fn_, __LINE__),                          \
        };                                                                    \
    static void KTEST_CONCAT(ktest_fn_, __LINE__)(struct ktest_ctx *ctx)

// Fails the current test and returns from it immediately. `ctx` is the
// parameter KTEST() declares, so these only work inside a KTEST body --
// deliberately, since a helper that can silently fail a test from three
// frames down is harder to read than one that returns a value.
#define KTEST_ASSERT(cond)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ktest_fail(ctx, #cond, __FILE__, __LINE__);                       \
            return;                                                           \
        }                                                                     \
    } while (0)

// Same, with both values reported -- worth the extra macro because
// "expected 4096, got 0" beats "assertion failed: got == 4096" when
// you're reading a CI log rather than sitting at the machine.
#define KTEST_ASSERT_EQ(got, expected)                                        \
    do {                                                                      \
        int64_t ktest_g_ = (int64_t)(got);                                    \
        int64_t ktest_e_ = (int64_t)(expected);                               \
        if (ktest_g_ != ktest_e_) {                                           \
            ktest_fail_eq(ctx, #got, ktest_g_, ktest_e_, __FILE__, __LINE__); \
            return;                                                           \
        }                                                                     \
    } while (0)

// Ends the test as SKIPPED, not passed and not failed. For a test whose
// preconditions aren't met on this boot -- the filesystem tests on a
// RAM-only boot, say. A skip is reported distinctly so "everything
// passed" can't quietly mean "nothing ran".
#define KTEST_SKIP(reason)                                                    \
    do {                                                                      \
        ktest_skip(ctx, reason);                                              \
        return;                                                               \
    } while (0)

void ktest_fail(struct ktest_ctx *ctx, const char *expr, const char *file, int line);
void ktest_fail_eq(struct ktest_ctx *ctx, const char *expr, int64_t got, int64_t expected,
                    const char *file, int line);
void ktest_skip(struct ktest_ctx *ctx, const char *reason);

#endif
