// Paints a KNOWN PATTERN on the physical console using ANSI cursor
// movement and erasing, then holds it still long enough to be
// photographed. tools/ansi_cursor_test.py reads the pixels.
//
// WHY A SEPARATE PROGRAM AND NOT A KTEST. ansi.c is a pure state
// machine and its KTESTs cover what a sequence RESOLVES to with no
// display at all. What they cannot cover is the other half -- whether
// vga.c then puts ink in the right cell -- and that is only observable
// as pixels.
//
// THE PATTERN IS BUILT FROM GAPS. Every check has a neighbour that must
// stay BLANK: a mark at column 5 with column 6 clear, a line erased
// from column 5 with columns 1-4 surviving. "Something was drawn"
// is satisfied by a console that ignores cursor movement entirely and
// prints everything in a row; only the gaps distinguish that from
// working.
//
// It SLEEPS while holding the pattern, which is why it must be spawned
// rather than `run` -- the legacy loader has no scheduler slot and
// sys_sleep_ms() returns -1 there. Without the pause the shell's own
// "process returned" line would scroll the pattern away before anything
// could look at it.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"

#define HOLD_MS 6000

static void esc(const char *s) { sys_write(1, s, strlen(s)); }

int main(void) {
    // Geometry to stderr (the kernel log, which reaches the serial
    // console) rather than stdout: stdout is the screen this is about
    // to paint, and text on it would be indistinguishable from the
    // pattern.
    char msg[96];

    esc("\033[2J");        // clear the whole screen
    esc("\033[?25l");      // hide the cursor -- a blinking block would be ink
                            // in a cell nothing asked to be painted

    // Two marks on one row with a GAP between them. A console that
    // ignored cursor movement would print "##" adjacent.
    esc("\033[3;5H#");
    esc("\033[3;9H#");

    // A run of eight, then erase from column 5 to the end of the line.
    // Columns 1-4 must survive and 5-8 must not.
    esc("\033[6;1H########");
    esc("\033[6;5H\033[K");

    // RELATIVE movement: home, then four columns right. Lands on
    // column 5 with 1-4 blank.
    esc("\033[9;1H\033[4C#");

    // An explicit ZERO means one, not none: this lands on row 11, and
    // a parser passing the zero through would leave it on row 12.
    esc("\033[12;3H\033[0A#");

    snprintf(msg, sizeof msg, "ansidraw: painted, holding for %d ms\n", HOLD_MS);
    sys_eprint(msg);

    sys_sleep_ms(HOLD_MS);

    esc("\033[?25h");      // put the cursor back for whoever comes next
    esc("\033[24;1H");
    sys_eprint("ansidraw: done\n");
    return 0;
}
