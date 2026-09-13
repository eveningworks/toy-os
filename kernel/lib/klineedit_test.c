// Tests for the line editor core. These are the reason it's a separate
// render-free library: "Alt-B from mid-word lands at the word's start"
// is one assertion here and a screenshot to squint at in either front
// end. The bash-fidelity details (Ctrl-W vs Alt-Backspace using
// different word definitions, Ctrl-U killing backwards only, Ctrl-D
// meaning two things) are exactly what a reimplementation gets subtly
// wrong, so each has its own test.
#include "ktest.h"
#include "klineedit.h"
#include "klineedit_cases.h"
#include "keyboard.h"
#include "string.h"
#include "heap.h"   // kmalloc/kfree -- the editor takes its memory from the caller
#include "fs.h"        // fs_exists -- the ring-3 half is a /tests binary
#include "scheduler.h" // scheduler_spawn/_poll, same shape as tty_test.c
#include "timer.h"     // pit_ticks -- the spawn deadline

// The ring-0 allocator, handed to the editor because klineedit.c cannot
// name one (see klineedit.h). The undo cases need it: a snapshot that
// cannot be allocated is dropped, so without this they would pass by
// asserting nothing.
static void *ktest_alloc(unsigned long n) { return kmalloc((uint32_t)n); }
static void ktest_free(void *p) { kfree(p); }
static const struct kline_mem ktest_mem = { ktest_alloc, ktest_free, 0 }; // 0: no ceiling


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
    kline_init_mem(&e, &ktest_mem);

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
    kline_init_mem(&e, &ktest_mem);
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

    kline_init_mem(&a, &ktest_mem);
    type(&a, "run /bin/ls");
    kline_key(&a, CTRL('w'));
    KTEST_ASSERT(eq(a.buf, "run ")); // the whole path went

    kline_init_mem(&b, &ktest_mem);
    type(&b, "run /bin/ls");
    meta(&b, '\b');
    KTEST_ASSERT(eq(b.buf, "run /bin/")); // only "ls" went
}

KTEST("klineedit", "kill to end, kill to start, and delete-forward") {
    struct kline_edit e;
    kline_init_mem(&e, &ktest_mem);

    type(&e, "hello world");
    kline_key(&e, CTRL('a'));
    for (int i = 0; i < 6; i++) kline_key(&e, CTRL('f')); // before "world"
    kline_key(&e, CTRL('k'));
    KTEST_ASSERT(eq(e.buf, "hello "));

    kline_init_mem(&e, &ktest_mem);
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
    kline_init_mem(&e, &ktest_mem);

    KTEST_ASSERT_EQ(kline_key(&e, CTRL('d')), KLINE_EOF);

    type(&e, "x");
    kline_key(&e, CTRL('a'));
    KTEST_ASSERT_EQ(kline_key(&e, CTRL('d')), KLINE_REDRAW); // deletes instead
    KTEST_ASSERT_EQ(e.len, 0);
}

KTEST("klineedit", "kill ring: yank, and yank-pop only after a yank") {
    struct kline_edit e;
    kline_init_mem(&e, &ktest_mem);

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

    kline_init_mem(&e, &ktest_mem);
    type(&e, "hello world");
    kline_key(&e, CTRL('a'));
    meta(&e, 'u');
    KTEST_ASSERT(eq(e.buf, "HELLO world"));
    meta(&e, 'c'); // capitalize the next word
    KTEST_ASSERT(eq(e.buf, "HELLO World"));
    kline_key(&e, CTRL('a'));
    meta(&e, 'l');
    KTEST_ASSERT(eq(e.buf, "hello World"));

    kline_init_mem(&e, &ktest_mem);
    type(&e, "ab");
    kline_key(&e, CTRL('t'));
    KTEST_ASSERT(eq(e.buf, "ba"));
}

KTEST("klineedit", "undo steps back through edits") {
    struct kline_edit e;
    kline_init_mem(&e, &ktest_mem);

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
    kline_init_mem(&e, &ktest_mem);

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
    kline_init_mem(&e, &ktest_mem);
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
    kline_init_mem(&e, &ktest_mem);
    type(&e, "typed");

    kline_set(&e, "recalled from history");
    KTEST_ASSERT(eq(e.buf, "recalled from history"));
    KTEST_ASSERT_EQ(e.cursor, e.len);

    kline_set(&e, "");
    KTEST_ASSERT_EQ(e.len, 0);
    KTEST_ASSERT_EQ(e.cursor, 0);
}

KTEST("klineedit", "with no allocator a full line refuses more input") {
    // The contract for a front end that passes NULL, and what this
    // editor did for its whole life: stop at the inline buffer and
    // REFUSE, never truncate somewhere the caller cannot see.
    struct kline_edit e;
    kline_init(&e);
    for (int i = 0; i < KLINE_INLINE + 20; i++) kline_key(&e, 'x');

    KTEST_ASSERT_EQ(e.len, KLINE_INLINE - 1);
    KTEST_ASSERT_EQ(e.buf[e.len], '\0');
    KTEST_ASSERT_EQ(e.cursor, e.len);
}

KTEST("klineedit", "Alt-T transposes words longer than the inline buffer") {
    // THE CASE THE OLD IMPLEMENTATION MANGLED. It staged the span in a
    // fixed 128-byte array and capped each copy at 127, so a span past
    // that was rewritten only in part and kept the ORIGINAL text in its
    // tail -- a silently wrong command line. The rotate has no buffer
    // and no length to cap.
    struct kline_edit e;
    kline_init_mem(&e, &ktest_mem);
    for (int i = 0; i < 100; i++) kline_key(&e, 'a');
    kline_key(&e, ' ');
    for (int i = 0; i < 100; i++) kline_key(&e, 'b');

    kline_key(&e, 0x1B); kline_key(&e, 't');   // Alt-T

    KTEST_ASSERT_EQ(e.len, 201);
    for (int i = 0; i < 100; i++) KTEST_ASSERT_EQ(e.buf[i], 'b');
    KTEST_ASSERT_EQ(e.buf[100], ' ');
    for (int i = 101; i < 201; i++) KTEST_ASSERT_EQ(e.buf[i], 'a');
    KTEST_ASSERT_EQ(e.buf[201], '\0');
    kline_free(&e);
}

KTEST("klineedit", "with an allocator the line grows past the inline buffer") {
    struct kline_edit e;
    kline_init_mem(&e, &ktest_mem);
    const int want = KLINE_INLINE * 4 + 7;   // several doublings, not a round one
    for (int i = 0; i < want; i++) kline_key(&e, 'x');

    KTEST_ASSERT_EQ(e.len, want);
    KTEST_ASSERT_EQ(e.buf[e.len], '\0');
    KTEST_ASSERT_EQ(e.cursor, e.len);
    // It really left the inline buffer, rather than growing a number.
    KTEST_ASSERT(e.buf != e.inln);
    KTEST_ASSERT(e.cap > KLINE_INLINE);

    // And every character survived the copies that growth made.
    for (int i = 0; i < want; i++) KTEST_ASSERT_EQ(e.buf[i], 'x');

    // Undo restores a line longer than the inline buffer -- the case a
    // fixed-size snapshot could not hold. Ctrl-W rather than a typed
    // character: typing does not snapshot per key (readline groups it),
    // so a delete is what actually pushes one.
    kline_key(&e, CTRL('w'));          // kills the whole run of x's
    KTEST_ASSERT_EQ(e.len, 0);
    kline_key(&e, 0x1F);               // Ctrl-_, readline's undo
    KTEST_ASSERT_EQ(e.len, want);
    for (int i = 0; i < want; i++) KTEST_ASSERT_EQ(e.buf[i], 'x');

    kline_free(&e);
    KTEST_ASSERT_EQ(e.cap, KLINE_INLINE);   // back to owning nothing
}

// The shared case table, asserted in the KERNEL's build of the editor.
// userland/tests/klineedit_test.c runs the identical table against the
// ring-3 build; a case that passes here and fails there means the two
// compilations of one source have diverged, which is the whole risk
// the shared-source rule takes on. See klineedit_cases.h.
KTEST("klineedit", "the shared case table holds in ring 0") {
    static struct kline_edit e; // ~1.2 KiB -- see kline_case_run()
    static char got[KLINE_MAX];
    int cursor = 0;
    for (int i = 0; i < kline_case_count; i++) {
        KTEST_ASSERT(kline_case_run(&kline_cases[i], &e, got, sizeof got, &cursor, &ktest_mem));
    }
    KTEST_ASSERT(kline_case_count >= 10); // the table is reachable, not empty
}

// --- the THIRD front end, and the only one with a vendored shell on it -
//
// /bin/dash ships without libedit, so this port had no arrow keys, no
// history and no `fc` until userland/backends/dash/histedit_shim.c
// answered libedit's names with the editor above. The claim is that
// dash EDITS with this code -- which cannot be checked from ring 0,
// because it needs a shell, a terminal and a keystroke.
//
// /tests/dashedit_test does it on a pty and reports by exit code; this
// drives it. A pty and not a pipe on purpose: dash builds an EditLine
// only when its input is interactive AND a terminal, so the pipe version
// of this test would measure a shell with editing switched off and pass
// whether or not any of it worked.
#define DASHEDIT_TEST_PATH "/tests/dashedit_test"
#define DASHEDIT_TIMEOUT_TICKS 900 // 9s at 100Hz -- it spawns a whole shell

KTEST("klineedit", "dash edits with this editor, over a pty") {
    if (!fs_exists(DASHEDIT_TEST_PATH)) KTEST_SKIP("no " DASHEDIT_TEST_PATH);
    if (!fs_exists("/bin/dash")) KTEST_SKIP("no /bin/dash on this boot");

    int pid = scheduler_spawn(DASHEDIT_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1, exited = 0;
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < DASHEDIT_TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // 0 = every phase worked; the other codes name WHICH link broke --
    // see userland/tests/dashedit_test.c. 7 is the one that matters:
    // Up did not bring the previous line back.
    KTEST_ASSERT_EQ(code, 0);
}
