// The SGR parser, with no display anywhere near it -- which is the
// point: vga.c drives this for the physical console AND for a GUI
// Terminal's scrollback, so the logic is worth asserting once, here,
// rather than through a screenshot of either.
#include "ktest.h"
#include "ansi.h"

// Feeds a whole string and reports what came out: how many bytes a
// caller would have PRINTED, and the colours left behind. That pair is
// the parser's entire contract.
static int feed(struct ansi_parser *p, const char *s, char *out, int cap) {
    int n = 0;
    for (const char *c = s; *c; c++) {
        if (ansi_feed(p, *c) == ANSI_PASS && n < cap - 1) out[n++] = *c;
    }
    out[n] = '\0';
    return n;
}

KTEST("ansi", "plain text passes through untouched") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[32];
    KTEST_ASSERT(feed(&p, "hello", out, sizeof out) == 5);
    KTEST_ASSERT(out[0] == 'h' && out[4] == 'o');
    KTEST_ASSERT(p.fg == VGA_LIGHT_GREY && p.bg == VGA_BLACK);
}

KTEST("ansi", "a colour sequence sets the colour and prints nothing") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[32];
    // ANSI 1 is RED, where VGA 1 is BLUE -- the mapping this asserts is
    // exactly the one that would be silently wrong if it were done with
    // arithmetic instead of a table.
    KTEST_ASSERT(feed(&p, "\033[31mred", out, sizeof out) == 3);
    KTEST_ASSERT(p.fg == VGA_RED);
    KTEST_ASSERT(p.bg == VGA_BLACK); // untouched by a foreground-only sequence
}

KTEST("ansi", "the eight colours map to VGA, not to VGA's bit order") {
    KTEST_ASSERT(ansi_color(0, 0) == VGA_BLACK);
    KTEST_ASSERT(ansi_color(1, 0) == VGA_RED);
    KTEST_ASSERT(ansi_color(2, 0) == VGA_GREEN);
    KTEST_ASSERT(ansi_color(3, 0) == VGA_BROWN);
    KTEST_ASSERT(ansi_color(4, 0) == VGA_BLUE);
    KTEST_ASSERT(ansi_color(6, 0) == VGA_CYAN);
    // Index 7 is LIGHT_GREY and not WHITE, so that its bright form is
    // white rather than running off the end of the 16-colour palette.
    KTEST_ASSERT(ansi_color(7, 0) == VGA_LIGHT_GREY);
    KTEST_ASSERT(ansi_color(7, 1) == VGA_WHITE);
}

KTEST("ansi", "bold applies to the foreground whichever order it arrives in") {
    struct ansi_parser p;
    char out[8];
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    feed(&p, "\033[1;31m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_LIGHT_RED);

    // The same two parameters the other way round. This is the case a
    // "brighten it when I see the 1" implementation gets wrong.
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    feed(&p, "\033[31;1m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_LIGHT_RED);
}

KTEST("ansi", "90-97 are bright without needing bold") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    feed(&p, "\033[96m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_LIGHT_CYAN);
    KTEST_ASSERT(p.bold == 0);
}

KTEST("ansi", "a background sequence leaves the foreground alone") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_CYAN, VGA_BLACK);
    char out[8];
    feed(&p, "\033[44m", out, sizeof out);
    KTEST_ASSERT(p.bg == VGA_BLUE);
    KTEST_ASSERT(p.fg == VGA_LIGHT_CYAN);
}

KTEST("ansi", "reset returns to the console defaults") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    feed(&p, "\033[1;41;36m", out, sizeof out);
    KTEST_ASSERT(p.fg != VGA_LIGHT_GREY && p.bg != VGA_BLACK);
    feed(&p, "\033[0m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_LIGHT_GREY && p.bg == VGA_BLACK && p.bold == 0);
}

KTEST("ansi", "a bare ESC[m is a reset, not a no-op") {
    // The empty-parameter case: a program writing the shortest possible
    // reset must not be treated as "no parameters, nothing to do".
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    feed(&p, "\033[31m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_RED);
    feed(&p, "\033[m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_LIGHT_GREY);
}

KTEST("ansi", "an unimplemented sequence is swallowed, not printed") {
    // THE POINT OF THE PARSER'S THIRD STATE. Without it, a program
    // clearing the screen would print "[2J" and a cursor move would
    // print "[10;5H" -- visible garbage that looks like a bug in the
    // program rather than a terminal declining to do something.
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[32];
    KTEST_ASSERT(feed(&p, "\033[2J", out, sizeof out) == 0);
    KTEST_ASSERT(feed(&p, "\033[10;5Hx", out, sizeof out) == 1);
    KTEST_ASSERT(out[0] == 'x');
    // A private sequence (the `?` form) is swallowed the same way, and
    // must NOT be mistaken for an SGR because it happens to end in 'm'.
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    KTEST_ASSERT(feed(&p, "\033[?25l", out, sizeof out) == 0);
    KTEST_ASSERT(p.fg == VGA_LIGHT_GREY);
}

KTEST("ansi", "an unknown parameter does not discard the ones beside it") {
    // `ESC[4;31m` -- underline, which this console has no way to show,
    // followed by red, which it does. Dropping the whole sequence
    // because one parameter is unsupported is the tempting bug.
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    feed(&p, "\033[4;31m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_RED);
}

KTEST("ansi", "a lone ESC does not print a glyph") {
    // Alt-<key> reaches a program as ESC followed by a letter
    // (keyboard.h), so this is not hypothetical: printing either byte
    // would put junk on screen every time somebody pressed Alt.
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    KTEST_ASSERT(feed(&p, "\033f", out, sizeof out) == 0);
    // ...and the parser is back in the ground state afterwards.
    KTEST_ASSERT(feed(&p, "ok", out, sizeof out) == 2);
}

KTEST("ansi", "a runaway parameter saturates instead of wrapping into a valid code") {
    // 99999999... must not arrive at, say, 31 by overflowing. Nonsense
    // in stays nonsense out.
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    feed(&p, "\033[99999999999m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_LIGHT_GREY); // unchanged -- not silently red
}

KTEST("ansi", "more parameters than the array holds does not run off it") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    feed(&p, "\033[1;2;3;4;5;6;7;8;9;10;11;12;31m", out, sizeof out);
    KTEST_ASSERT(p.nparam == 0); // consumed and reset, no overrun
}

// --- cursor and erase ------------------------------------------------
//
// These assert the PARSER's resolution, not what appears on screen:
// ansi.c knows nothing about a display, which is what lets it be tested
// with no hardware at all. What vga.c does with an op is checked by
// tools/ansi_cursor_test.py, which reads pixels.

// Feeds a sequence and returns the op it resolved to, or ANSI_OP_NONE
// if it did not produce an ANSI_CTRL at all.
static enum ansi_op ctrl_op(struct ansi_parser *p, const char *seq) {
    p->op = ANSI_OP_NONE;
    for (const char *s = seq; *s; s++)
        if (ansi_feed(p, *s) == ANSI_CTRL) return p->op;
    return ANSI_OP_NONE;
}

KTEST("ansi", "cursor movement resolves to an op with its arguments") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);

    KTEST_ASSERT(ctrl_op(&p, "\033[5;9H") == ANSI_OP_MOVE_TO);
    KTEST_ASSERT(p.a == 5 && p.b == 9);
    // `f` is the same operation as `H`, and programs use both.
    KTEST_ASSERT(ctrl_op(&p, "\033[2;3f") == ANSI_OP_MOVE_TO);
    KTEST_ASSERT(p.a == 2 && p.b == 3);
    // A bare CUP means home, and BOTH defaults have to appear.
    KTEST_ASSERT(ctrl_op(&p, "\033[H") == ANSI_OP_MOVE_TO);
    KTEST_ASSERT(p.a == 1 && p.b == 1);

    KTEST_ASSERT(ctrl_op(&p, "\033[3A") == ANSI_OP_UP && p.a == 3);
    KTEST_ASSERT(ctrl_op(&p, "\033[B") == ANSI_OP_DOWN && p.a == 1);
    KTEST_ASSERT(ctrl_op(&p, "\033[7C") == ANSI_OP_RIGHT && p.a == 7);
    KTEST_ASSERT(ctrl_op(&p, "\033[2D") == ANSI_OP_LEFT && p.a == 2);
    KTEST_ASSERT(ctrl_op(&p, "\033[12G") == ANSI_OP_COLUMN && p.a == 12);
    KTEST_ASSERT(ctrl_op(&p, "\033[4d") == ANSI_OP_ROW && p.a == 4);
}

KTEST("ansi", "an explicit zero means the DEFAULT, not zero") {
    // ANSI's rule, and the one that is invisible until a program emits
    // the zero form: `ESC[0A` moves up ONE line. A parser that passed
    // the zero through would move none and the display would look
    // frozen for that program only.
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    KTEST_ASSERT(ctrl_op(&p, "\033[0A") == ANSI_OP_UP && p.a == 1);
    KTEST_ASSERT(ctrl_op(&p, "\033[0;0H") == ANSI_OP_MOVE_TO);
    KTEST_ASSERT(p.a == 1 && p.b == 1);
}

KTEST("ansi", "erase defaults to mode 0, the opposite end from a movement") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    // A bare ED/EL is "cursor to end" -- mode 0. Applying a movement's
    // "absent means 1" rule here would give mode 1 and erase the wrong
    // half of the screen.
    KTEST_ASSERT(ctrl_op(&p, "\033[J") == ANSI_OP_ERASE_DISPLAY && p.a == 0);
    KTEST_ASSERT(ctrl_op(&p, "\033[K") == ANSI_OP_ERASE_LINE && p.a == 0);
    KTEST_ASSERT(ctrl_op(&p, "\033[2J") == ANSI_OP_ERASE_DISPLAY && p.a == 2);
    KTEST_ASSERT(ctrl_op(&p, "\033[1K") == ANSI_OP_ERASE_LINE && p.a == 1);
    // Mode 3 is "and the scrollback", reported as 2: the history is the
    // user's, not the program's, to throw away.
    KTEST_ASSERT(ctrl_op(&p, "\033[3J") == ANSI_OP_ERASE_DISPLAY && p.a == 2);
    // An out-of-range mode is not an erase at all.
    KTEST_ASSERT(ctrl_op(&p, "\033[9J") == ANSI_OP_NONE);
}

KTEST("ansi", "the private sequences that do something: DECTCEM and 1049") {
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    KTEST_ASSERT(ctrl_op(&p, "\033[?25l") == ANSI_OP_HIDE);
    KTEST_ASSERT(ctrl_op(&p, "\033[?25h") == ANSI_OP_SHOW);

    // THE ALTERNATE SCREEN. This assertion used to be the OPPOSITE --
    // that 1049 stayed swallowed, "which this console has no answer
    // for" -- and it was right when it was written. The GUI Terminal
    // has an answer now (a saved grid), which is what lets `less` and
    // `edit` leave a terminal exactly as they found it. A consumer that
    // still has no second screen ignores the op, so the physical
    // console is unaffected.
    KTEST_ASSERT(ctrl_op(&p, "\033[?1049h") == ANSI_OP_ALT_ON);
    KTEST_ASSERT(ctrl_op(&p, "\033[?1049l") == ANSI_OP_ALT_OFF);

    // **ONLY 1049.** 47 and 1047 are the older spellings that do not
    // carry the cursor; one spelling means a consumer has one thing to
    // implement, and a program sending the old pair gets what any
    // terminal without them gives it.
    KTEST_ASSERT(ctrl_op(&p, "\033[?47h") == ANSI_OP_NONE);
    KTEST_ASSERT(ctrl_op(&p, "\033[?1047h") == ANSI_OP_NONE);
    KTEST_ASSERT(ctrl_op(&p, "\033[?7l") == ANSI_OP_NONE);
}

KTEST("ansi", "SGR still works, and does not resolve to a control op") {
    // The regression that matters: cursor support added a second exit
    // from the same final-byte branch, and a colour sequence must not
    // take it.
    struct ansi_parser p;
    ansi_init(&p, VGA_LIGHT_GREY, VGA_BLACK);
    char out[8];
    feed(&p, "\033[31m", out, sizeof out);
    KTEST_ASSERT(p.fg == VGA_RED);
    KTEST_ASSERT(ctrl_op(&p, "\033[32m") == ANSI_OP_NONE);
}
