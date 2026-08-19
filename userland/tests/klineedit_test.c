// The line editor, in RING 3.
//
// WHY THIS EXISTS RATHER THAN ONLY A KTEST -- the same reason
// libc_test.c does. kernel/lib/klineedit.c has KTESTs covering its
// logic, and they would go on passing whether or not a single line of
// it were reachable from a ring-3 program. What is new is the LINK:
// klineedit.c is compiled a second time with USERLAND_CFLAGS into
// libuapp.a so /bin/tosh and the GUI Terminal can share one definition
// of what Ctrl-A means, and a KTEST runs inside the kernel and cannot
// see that build at all.
//
// So the assertion is deliberately not "the editor works". It is "the
// ring-3 build of the editor produces byte-for-byte what the kernel's
// build produces", over the table both rings read
// (kernel/include/api/klineedit_cases.h). A case is added once and both
// rings gain it.
//
// Prints one line per case and exits with the number of failures, so
// `run klineedit_test` reports 0 when everything holds.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "klineedit.h"
#include "klineedit_cases.h"

static int g_fail;

static void put(const char *s) { sys_write(1, s, strlen(s)); }

int main(void) {
    put("klineedit_test: the shared line editor, built for ring 3\n");

    // Static: struct kline_edit is ~1.2 KiB, which is why
    // kline_case_run() takes it rather than holding one --
    // USERLAND_CFLAGS carries -Wframe-larger-than=2048 and a big local
    // array in ring 3 steps over the single guard page.
    static struct kline_edit ed;
    static char got[KLINE_MAX];
    char msg[160];

    for (int i = 0; i < kline_case_count; i++) {
        int cursor = 0;
        int ok = kline_case_run(&kline_cases[i], &ed, got, sizeof got, &cursor);
        if (ok) {
            snprintf(msg, sizeof msg, "  ok   %s\n", kline_cases[i].name);
        } else {
            // Say what it got, not just that it differed: a mismatch
            // here is a two-compilations question, and the actual bytes
            // are the first thing anyone will want.
            snprintf(msg, sizeof msg,
                     "  FAIL %s -- got \"%s\"@%d want \"%s\"@%d\n",
                     kline_cases[i].name, got, cursor,
                     kline_cases[i].want, kline_cases[i].want_cursor);
            g_fail++;
        }
        put(msg);
    }

    // The table being EMPTY would print nothing and exit 0, which reads
    // exactly like every case passing -- the failure mode this repo
    // keeps rediscovering. Assert it was reachable and populated.
    if (kline_case_count < 10) {
        snprintf(msg, sizeof msg,
                 "  FAIL only %d cases -- the shared table is not linked\n",
                 kline_case_count);
        put(msg);
        g_fail++;
    }

    if (g_fail == 0) put("klineedit_test: all checks passed\n");
    return g_fail;
}
