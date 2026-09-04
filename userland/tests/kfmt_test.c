// The shared kfmt case table, run in RING 3 through libc.a's copy.
//
// The KTEST beside it (kernel/lib/kfmt_test.c) runs the identical
// cases, so what this adds is not coverage of the logic -- it is proof
// that ring 3 has the same formatter at all. kfmt.c is compiled twice
// and its header is one file over two implementations (kfmt.c shared,
// kfmt_print.c kernel-only), so a kernel include creeping into the
// shared half takes snprintf away from userland without any KTEST
// noticing. This is the same gap userland/tests/klineedit_test.c exists
// for.
//
// It goes through <stdio.h>'s snprintf rather than k_snprintf, because
// that is what a ported program calls and therefore what has to be
// right. The case table's runner uses k_snprintf; both resolve to the
// same object in libc.a, and if they ever stop doing so this test is
// where it shows up.
#include "rt/sys.h"
#include "kfmt_cases.h"
#include <stdio.h>
#include <string.h>
#include "lib/utest.h"

int main(void) {
    char got[128];

    // UTEST_KLOG because a KTEST spawns this with no terminal and reads
    // the report back from `dmesg`; UTEST_QUIET because the table runs
    // to hundreds of cases and the epilogue's count is the evidence.
    utest_begin("kfmt_test", "the shared kfmt case table, in ring 3",
                UTEST_KLOG | UTEST_QUIET);

    for (int i = 0; i < kfmt_case_count; i++) {
        utest_checkf(kfmt_case_run(&kfmt_cases[i], got, sizeof got),
                     "\"%s\" gave \"%s\", wanted \"%s\"",
                     kfmt_cases[i].fmt, got, kfmt_cases[i].want);
    }

    // The SAME assertion made through <stdio.h>'s own entry point, so a
    // libc whose snprintf had drifted from k_snprintf -- a second
    // implementation, a wrapper that lost a flag -- would be caught
    // here rather than agreeing with itself.
    snprintf(got, sizeof got, "U+%04X slot %d", 0x67, 71);
    utest_checkf(strcmp(got, "U+0067 slot 71") == 0,
                 "stdio snprintf gave \"%s\"", got);

    // A table that has been emptied must fail rather than pass
    // vacuously -- the whole value here is breadth. (utest_end() also
    // refuses a run with no checks at all, which is the same rule one
    // step further out.)
    utest_checkf(kfmt_case_count >= 40,
                 "only %d cases in the shared table", kfmt_case_count);

    return utest_end();
}
