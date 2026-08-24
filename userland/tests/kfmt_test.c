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

int main(void) {
    char line[192], got[128];
    int fails = 0;

    for (int i = 0; i < kfmt_case_count; i++) {
        if (kfmt_case_run(&kfmt_cases[i], got, sizeof got)) continue;
        fails++;
        snprintf(line, sizeof line,
                 "kfmt: FAIL \"%s\" gave \"%s\", wanted \"%s\"\n",
                 kfmt_cases[i].fmt, got, kfmt_cases[i].want);
        sys_eprint(line);
    }

    // The SAME assertion made through <stdio.h>'s own entry point, so a
    // libc whose snprintf had drifted from k_snprintf -- a second
    // implementation, a wrapper that lost a flag -- would be caught
    // here rather than agreeing with itself.
    snprintf(got, sizeof got, "U+%04X slot %d", 0x67, 71);
    if (strcmp(got, "U+0067 slot 71") != 0) {
        fails++;
        snprintf(line, sizeof line,
                 "kfmt: FAIL stdio snprintf gave \"%s\"\n", got);
        sys_eprint(line);
    }

    // A table that has been emptied must fail rather than pass
    // vacuously -- the whole value here is breadth.
    if (kfmt_case_count < 40) {
        fails++;
        snprintf(line, sizeof line,
                 "kfmt: FAIL only %d cases in the shared table\n",
                 kfmt_case_count);
        sys_eprint(line);
    }

    snprintf(line, sizeof line, "kfmt: %d/%d cases passed\n",
             kfmt_case_count - fails, kfmt_case_count);
    sys_eprint(line);
    return fails ? 1 : 0;
}
