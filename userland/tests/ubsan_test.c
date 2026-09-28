// The shared UBSAN case table, run in RING 3 through libc's copy of the
// runtime (kernel/lib/ubsan.c, compiled twice) -- the kernel's KTEST
// runs the identical rows. Without this, ring 3's copy is exercised only
// by a UBSAN=1 build, and only when something is actually wrong.
//
// In a UBSAN=1 build it also commits one real signed overflow, the
// positive control that the flags reached userland at all.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "ubsan.h"
#include "ubsan_cases.h"
#include "lib/utest.h"

int main(void) {
    char got[256];
    utest_begin("ubsan_test", "the shared UBSAN case table, in ring 3", UTEST_QUIET);

    for (int i = 0; i < ubsan_case_count; i++) {
        utest_checkf(ubsan_case_run(&ubsan_cases[i], got, sizeof got),
                     "case gave \"%s\", wanted \"%s\"", got, ubsan_cases[i].want);
    }
    utest_checkf(ubsan_case_count >= 20, "only %d cases in the shared table",
                 ubsan_case_count);
    utest_checkf(ubsan_case_once(), "a site that failed twice did not report once");

#ifdef TOYOS_UBSAN
    volatile int big = 0x7fffffff, one = 1;
    unsigned before = ubsan_report_count();
    volatile int sum = big + one;
    (void)sum;
    utest_checkf(ubsan_report_count() == before + 1,
                 "a real signed overflow made %u reports, not 1",
                 ubsan_report_count() - before);
    utest_checkf(strstr(ubsan_last_report(), "userland/tests/ubsan_test.c") != NULL,
                 "the report does not name this file: \"%s\"", ubsan_last_report());
#endif
    return utest_end();
}
