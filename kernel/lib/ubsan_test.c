// Tests for the UBSAN runtime (kernel/lib/ubsan.c). The case table runs
// in every build; the last test needs `make UBSAN=1` and skips without
// it. Its twin in ring 3 is /tests/ubsan_test.
#include "ktest.h"
#include "ubsan.h"
#include "ubsan_cases.h"
#include "kfmt.h"
#include "string.h"

KTEST("ubsan", "every handler's report in the shared case table") {
    char got[256];
    for (int i = 0; i < ubsan_case_count; i++) {
        if (ubsan_case_run(&ubsan_cases[i], got, sizeof got)) continue;
        klog_printf("ubsan: case gave \"%s\", wanted \"%s\"\n", got, ubsan_cases[i].want);
        KTEST_ASSERT(0);
    }
    // An emptied table must fail rather than pass vacuously.
    KTEST_ASSERT(ubsan_case_count >= 20);
}

KTEST("ubsan", "a site that fails twice reports once") {
    KTEST_ASSERT(ubsan_case_once());
}

// The POSITIVE CONTROL for the whole build: a real signed overflow in
// instrumented code must reach the runtime and name this file. It fails
// if the flags never reached kernel/, or the exclusion list swallowed it.
KTEST("ubsan", "an instrumented kernel reports a real signed overflow") {
#ifndef TOYOS_UBSAN
    KTEST_SKIP("not a UBSAN=1 build");
#else
    static int fired;   // the site reports once per boot, so once per boot
    if (fired) KTEST_SKIP("already reported on this boot");
    fired = 1;
    volatile int big = 0x7fffffff, one = 1;
    unsigned before = ubsan_report_count();
    volatile int sum = big + one;
    (void)sum;
    KTEST_ASSERT_EQ(ubsan_report_count(), before + 1);
    KTEST_ASSERT(k_strstr(ubsan_last_report(), "kernel/lib/ubsan_test.c") != 0);
    KTEST_ASSERT(k_strstr(ubsan_last_report(), "signed-integer-overflow") != 0);
#endif
}
