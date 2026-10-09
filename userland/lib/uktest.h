#ifndef ULIB_UKTEST_H
#define ULIB_UKTEST_H
// KTEST's assertions in ring 3, so a test can follow its code out of the
// kernel with its bodies unchanged: a case is a plain function, and
// KTEST_ASSERT / KTEST_ASSERT_EQ fail it and return, exactly as in
// kernel/include/kernel/ktest.h. A table of cases replaces KTEST()'s
// linker section. Reports through lib/utest.h, one line per case.
//
// Header-only for utest.h's reason: a test links only what it calls.
#include <stdint.h>
#include "lib/utest.h"

struct uktest_case {
    const char *name;
    void (*fn)(void);
};

static const char *uktest_current;
static int uktest_case_failed;

static inline void uktest_fail(const char *expr, int line) {
    uktest_case_failed = 1;
    utest_checkf(0, "%s -- line %d: %s", uktest_current, line, expr);
}

static inline void uktest_fail_eq(const char *expr, int64_t got, int64_t want, int line) {
    uktest_case_failed = 1;
    utest_checkf(0, "%s -- line %d: %s is %lld, want %lld", uktest_current, line,
                 expr, (long long)got, (long long)want);
}

#define KTEST_ASSERT(cond)                                                    \
    do {                                                                      \
        if (!(cond)) { uktest_fail(#cond, __LINE__); return; }                \
    } while (0)

#define KTEST_ASSERT_EQ(got, expected)                                        \
    do {                                                                      \
        int64_t uktest_g_ = (int64_t)(got);                                   \
        int64_t uktest_e_ = (int64_t)(expected);                              \
        if (uktest_g_ != uktest_e_) {                                         \
            uktest_fail_eq(#got, uktest_g_, uktest_e_, __LINE__);             \
            return;                                                           \
        }                                                                     \
    } while (0)

// Runs every case between the caller's utest_begin() and utest_end().
static inline void uktest_run(const struct uktest_case *cases, int n) {
    for (int i = 0; i < n; i++) {
        uktest_current = cases[i].name;
        uktest_case_failed = 0;
        cases[i].fn();
        if (!uktest_case_failed) utest_check(1, cases[i].name);
    }
}

#endif
