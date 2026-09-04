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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "klineedit.h"
#include "klineedit_cases.h"

#include "lib/utest.h"

// Ring 3's allocator, for the same reason the kernel driver supplies
// one: the undo cases need memory and are silently skipped without it.
static void *u_alloc(unsigned long n) { return malloc((size_t)n); }
static void u_free(void *p) { free(p); }
static const struct kline_mem u_mem = { u_alloc, u_free, 0 }; // 0: no ceiling

int main(void) {
    utest_begin("klineedit_test", "the shared line editor, built for ring 3", 0);

    // Static: struct kline_edit is ~1.2 KiB, which is why
    // kline_case_run() takes it rather than holding one --
    // USERLAND_CFLAGS carries -Wframe-larger-than=2048 and a big local
    // array in ring 3 steps over the single guard page.
    static struct kline_edit ed;
    static char got[KLINE_MAX];
    char detail[160];

    for (int i = 0; i < kline_case_count; i++) {
        int cursor = 0;
        int ok = kline_case_run(&kline_cases[i], &ed, got, sizeof got, &cursor, &u_mem);
        // Say what it got, not just that it differed: a mismatch here is
        // a two-compilations question, and the actual bytes are the
        // first thing anyone will want. The detail is printed only on a
        // failure (utest_check_detail).
        snprintf(detail, sizeof detail, "got \"%s\"@%d want \"%s\"@%d",
                 got, cursor, kline_cases[i].want, kline_cases[i].want_cursor);
        utest_check_detail(ok, kline_cases[i].name, detail);
    }

    // The table being EMPTY would print nothing and exit 0, which reads
    // exactly like every case passing -- the failure mode this repo
    // keeps rediscovering. Assert it was reachable and populated.
    utest_checkf(kline_case_count >= 10,
                 "only %d cases -- the shared table is not linked",
                 kline_case_count);

    return utest_end();
}
