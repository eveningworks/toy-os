// Tests for the line editor core. These are the reason it's a separate
// render-free library: "Alt-B from mid-word lands at the word's start"
// is one assertion here and a screenshot to squint at in either front
// end. The bash-fidelity details (Ctrl-W vs Alt-Backspace using
// different word definitions, Ctrl-U killing backwards only, Ctrl-D
// meaning two things) are exactly what a reimplementation gets subtly
// wrong, so each has its own test.
#include "ktest.h"
#include "klineedit.h"
#include "keyboard.h"
#include "string.h"

#define CTRL(c) ((c) - 'a' + 1)
#define ESC 0x1B

static int eq(const char *a, const char *b) { return k_strcmp(a, b) == 0; }

// Types a literal string in, one key at a time.
static void type(struct kline_edit *e, const char *s) {
    for (; *s; s++) kline_key(e, *s);
}

// Feeds a Meta binding the way the driver encodes it: ESC then the key.
static enum kline_action meta(struct kline_edit *e, int key) {
    kline_key(e, ESC);
    return kline_key(e, key);
}

KTEST("klineedit", "insert at the cursor, not just at the end") {
    struct kline_edit e;
    kline_init(&e);

    type(&e, "helo");
    KTEST_ASSERT(eq(e.buf, "helo"));
    KTEST_ASSERT_EQ(e.cursor, 4);

    kline_key(&e, KEY_ARROW_LEFT); // between 'l' and 'o'
    KTEST_ASSERT_EQ(e.cursor, 3);
    kline_key(&e, 'l');
    KTEST_ASSERT(eq(e.buf, "hello"));
    KTEST_ASSERT_EQ(e.cursor, 4); // stays just after what was inserted
}

KTEST("klineedit", "motion: char, line ends, and words") {
    struct kline_edit e;
    kline_init(&e);
    type(&e, "cat /etc/toyos.conf");

    KTEST_ASSERT_EQ(kline_key(&e, CTRL('a')), KLINE_REDRAW);
    KTEST_ASSERT_EQ(e.cursor, 0);
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('a')), KLINE_IGNORED); // already there
    kline_key(&e, CTRL('e'));
    KTEST_ASSERT_EQ(e.cursor, e.len);

    kline_key(&e, CTRL('b'));
    KTEST_ASSERT_EQ(e.cursor, e.len - 1);
    kline_key(&e, CTRL('f'));
    KTEST_ASSERT_EQ(e.cursor, e.len);

    // Alt-B skips back over "conf", then the '.' separator, to "toyos".
    meta(&e, 'b');
    KTEST_ASSERT_EQ(e.cursor, 15); // start of "conf"
    meta(&e, 'b');
    KTEST_ASSERT_EQ(e.cursor, 9);  // start of "toyos"

    meta(&e, 'f');
    KTEST_ASSERT_EQ(e.cursor, 14); // end of "toyos"

    // Ctrl+arrows are the same motions.
    kline_key(&e, KEY_CTRL_ARROW_LEFT);
    KTEST_ASSERT_EQ(e.cursor, 9);
    kline_key(&e, KEY_CTRL_ARROW_RIGHT);
    KTEST_ASSERT_EQ(e.cursor, 14);
}

KTEST("klineedit", "Ctrl-W and Alt-Backspace use different word rules") {
    // This is real bash behavior, not an inconsistency: Ctrl-W is
    // whitespace-delimited (unix-word-rubout), Alt-Backspace is
    // alphanumeric (backward-kill-word).
    struct kline_edit a, b;

    kline_init(&a);
    type(&a, "run /bin/ls");
    kline_key(&a, CTRL('w'));
    KTEST_ASSERT(eq(a.buf, "run ")); // the whole path went

    kline_init(&b);
    type(&b, "run /bin/ls");
    meta(&b, '\b');
    KTEST_ASSERT(eq(b.buf, "run /bin/")); // only "ls" went
}

KTEST("klineedit", "kill to end, kill to start, and delete-forward") {
    struct kline_edit e;
    kline_init(&e);

    type(&e, "hello world");
    kline_key(&e, CTRL('a'));
    for (int i = 0; i < 6; i++) kline_key(&e, CTRL('f')); // before "world"
    kline_key(&e, CTRL('k'));
    KTEST_ASSERT(eq(e.buf, "hello "));

    kline_init(&e);
    type(&e, "hello world");
    for (int i = 0; i < 5; i++) kline_key(&e, CTRL('b')); // before "world"
    kline_key(&e, CTRL('u')); // backwards only -- NOT the whole line
    KTEST_ASSERT(eq(e.buf, "world"));
    KTEST_ASSERT_EQ(e.cursor, 0);

    kline_key(&e, KEY_DELETE);
    KTEST_ASSERT(eq(e.buf, "orld"));
    kline_key(&e, CTRL('d'));
    KTEST_ASSERT(eq(e.buf, "rld"));
}

KTEST("klineedit", "Ctrl-D means end-of-input only on an empty line") {
    struct kline_edit e;
    kline_init(&e);

    KTEST_ASSERT_EQ(kline_key(&e, CTRL('d')), KLINE_EOF);

    type(&e, "x");
    kline_key(&e, CTRL('a'));
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('d')), KLINE_REDRAW); // deletes instead
    KTEST_ASSERT_EQ(e.len, 0);
}

KTEST("klineedit", "kill ring: yank, and yank-pop only after a yank") {
    struct kline_edit e;
    kline_init(&e);

    type(&e, "alpha beta");
    kline_key(&e, CTRL('w')); // kills "beta"
    kline_key(&e, CTRL('w')); // kills "alpha "
    KTEST_ASSERT_EQ(e.len, 0);

    kline_key(&e, CTRL('y')); // yanks the most recent kill
    KTEST_ASSERT(eq(e.buf, "alpha "));

    meta(&e, 'y'); // yank-pop rotates to the older one
    KTEST_ASSERT(eq(e.buf, "beta"));

    // Not directly after a yank -> ignored, exactly as readline does.
    kline_key(&e, 'z');
    KTEST_ASSERT_EQ(meta(&e, 'y'), KLINE_IGNORED);
}

KTEST("klineedit", "case-changing a word, and transpose") {
    struct kline_edit e;

    kline_init(&e);
    type(&e, "hello world");
    kline_key(&e, CTRL('a'));
    meta(&e, 'u');
    KTEST_ASSERT(eq(e.buf, "HELLO world"));
    meta(&e, 'c'); // capitalize the next word
    KTEST_ASSERT(eq(e.buf, "HELLO World"));
    kline_key(&e, CTRL('a'));
    meta(&e, 'l');
    KTEST_ASSERT(eq(e.buf, "hello World"));

    kline_init(&e);
    type(&e, "ab");
    kline_key(&e, CTRL('t'));
    KTEST_ASSERT(eq(e.buf, "ba"));
}

KTEST("klineedit", "undo steps back through edits") {
    struct kline_edit e;
    kline_init(&e);

    type(&e, "hello world");
    kline_key(&e, CTRL('w')); // "hello "
    KTEST_ASSERT(eq(e.buf, "hello "));
    kline_key(&e, CTRL('w')); // ""
    KTEST_ASSERT_EQ(e.len, 0);

    kline_key(&e, 0x1F); // Ctrl-_
    KTEST_ASSERT(eq(e.buf, "hello "));
    kline_key(&e, 0x1F);
    KTEST_ASSERT(eq(e.buf, "hello world"));
    kline_key(&e, 0x1F); // nothing left to undo -- must not corrupt
    KTEST_ASSERT(eq(e.buf, "hello world"));
}

KTEST("klineedit", "the actions a front end has to handle") {
    struct kline_edit e;
    kline_init(&e);

    KTEST_ASSERT_EQ(kline_key(&e, '\r'), KLINE_ACCEPT);
    KTEST_ASSERT_EQ(kline_key(&e, '\n'), KLINE_ACCEPT);
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('c')), KLINE_CANCEL);
    KTEST_ASSERT_EQ(kline_key(&e, '\t'), KLINE_COMPLETE);
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('l')), KLINE_CLEAR_SCREEN);
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('r')), KLINE_SEARCH);
    KTEST_ASSERT_EQ(kline_key(&e, KEY_ARROW_UP), KLINE_HISTORY_PREV);
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('p')), KLINE_HISTORY_PREV);
    KTEST_ASSERT_EQ(kline_key(&e, KEY_ARROW_DOWN), KLINE_HISTORY_NEXT);
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('n')), KLINE_HISTORY_NEXT);
    KTEST_ASSERT_EQ(meta(&e, '.'), KLINE_LAST_ARG);
}

KTEST("klineedit", "a lone Esc is swallowed, not treated as a character") {
    struct kline_edit e;
    kline_init(&e);
    type(&e, "ab");

    // ESC alone changes nothing visible -- it waits for the next key to
    // find out whether it was Meta (see keyboard.h on the encoding).
    KTEST_ASSERT_EQ(kline_key(&e, ESC), KLINE_IGNORED);
    KTEST_ASSERT(eq(e.buf, "ab"));

    // An unrecognised Meta binding is ignored, and must not leave the
    // pending state armed for the key after it.
    KTEST_ASSERT_EQ(kline_key(&e, 'q'), KLINE_IGNORED);
    kline_key(&e, 'z');
    KTEST_ASSERT(eq(e.buf, "abz"));
}

KTEST("klineedit", "kline_set replaces the line and parks at the end") {
    struct kline_edit e;
    kline_init(&e);
    type(&e, "typed");

    kline_set(&e, "recalled from history");
    KTEST_ASSERT(eq(e.buf, "recalled from history"));
    KTEST_ASSERT_EQ(e.cursor, e.len);

    kline_set(&e, "");
    KTEST_ASSERT_EQ(e.len, 0);
    KTEST_ASSERT_EQ(e.cursor, 0);
}

KTEST("klineedit", "a full line refuses more input rather than corrupting") {
    struct kline_edit e;
    kline_init(&e);
    for (int i = 0; i < KLINE_MAX + 20; i++) kline_key(&e, 'x');

    KTEST_ASSERT_EQ(e.len, KLINE_MAX - 1);
    KTEST_ASSERT_EQ(e.buf[e.len], '\0');
    KTEST_ASSERT_EQ(e.cursor, e.len);
}
