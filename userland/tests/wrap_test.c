// uui_label's word wrapping, asserted directly.
//
// WHY THIS IS A RING-3 TEST AND NOT A KTEST. Wrapping measures text
// with ugfx_text_fit_chars(), which needs a loaded FONT -- and the font
// lives in ring 3's copy of the toolkit (ugfx_font_init(), see uapp.c's
// note about what a ring-0 component loses when it becomes a process).
// A KTEST runs inside the kernel and cannot reach any of it.
//
// THE CHECK THAT MATTERS IS THE THIRD ONE. A word wider than the whole
// line has no word boundary to break at, and the natural implementation
// -- "back up to the last space" -- backs up to the start and returns
// the same pointer it was given. That loops forever INSIDE A DRAW CALL,
// which does not draw a wrong pixel, it hangs the compositor. Every
// case below therefore also asserts that the cursor MOVED.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "ui/ugfx.h"
#include "ui/uui_label.h"

#include "lib/utest.h"

// The call sites here read `ok(name, cond, detail)`; the harness takes
// the boolean first. One adapter rather than transposing every call
// site: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void ok(const char *name, int cond, const char *detail) {
    utest_check_detail(cond, name, detail);
}

int main(int argc, char **argv) {
    // AND THE VERDICT TO A FILE (UTEST_VERDICT_FILE). This test has to
    // be SPAWNED rather than `run` (the font needs a real scheduler slot
    // -- see the top of this file), and a spawned program's console
    // output arrives while the harness is between commands, where it is
    // dropped. A file is an artifact the harness can ask for whenever it
    // likes, which is this repo's own rule about waiting on the artifact
    // rather than on the timing.
    utest_begin("wrap_test", "uui_label word wrapping", UTEST_VERDICT_FILE);

    (void)argc; (void)argv;
    // Without this every measurement is zero and every check below
    // passes for the wrong reason -- see uapp.c on the font not being
    // free in ring 3.
    ugfx_font_init();
    ok("the font loaded, so widths are real", ugfx_char_w() > 0, "char_w is 0");

    char line[128];
    int wide = ugfx_char_w() * 20;   // room for ~20 characters

    // --- breaks at a space, not mid-word ---
    const char *src = "the quick brown fox jumps over the lazy dog";
    const char *next = uui_label_wrap_next(src, wide, line, sizeof line);
    ok("a line fits within the width",
       ugfx_text_width(line) <= wide, line);
    ok("it breaks at a word boundary",
       *next == '\0' || *next == ' ' || next[-1] == ' ' || src[0] == '\0', line);
    ok("it consumed something", next > src, "cursor did not move");

    // --- the whole string when it fits ---
    next = uui_label_wrap_next("short", wide, line, sizeof line);
    ok("a string that fits comes back whole", strcmp(line, "short") == 0, line);
    ok("...and the cursor reaches the end", *next == '\0', next);

    // --- THE HANG CASE: one word wider than the line ---
    const char *big = "supercalifragilisticexpialidociousandthensome";
    int narrow = ugfx_char_w() * 6;
    next = uui_label_wrap_next(big, narrow, line, sizeof line);
    ok("an over-long word is BROKEN, not refused", next > big, "cursor did not move");
    ok("...and the piece still fits the line",
       ugfx_text_width(line) <= narrow, line);

    // Walking it to the end must terminate. Bounded so a regression
    // FAILS here instead of hanging this test the way it would hang the
    // compositor -- a test that reproduces the bug by never finishing
    // is not a useful test.
    int guard = 0;
    const char *p = big;
    while (*p && guard < 200) {
        const char *q = uui_label_wrap_next(p, narrow, line, sizeof line);
        if (q <= p) break;              // no progress: the bug
        p = q;
        guard++;
    }
    ok("wrapping an over-long word terminates", *p == '\0' && guard < 200,
       "did not reach the end of the string");

    // --- a leading space does not produce an empty line ---
    const char *indented = "   indented text";
    next = uui_label_wrap_next(indented, wide, line, sizeof line);
    ok("leading spaces are skipped", line[0] == 'i', line);
    ok("...and it still advanced", next > indented, "cursor did not move");

    return utest_end();
}
