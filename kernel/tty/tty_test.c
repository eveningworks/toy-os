// The console's reported state, against the state itself.
//
// READ-ONLY ON PURPOSE. A KTEST runs in the LIVE kernel, so a test that
// claimed the console or moved the foreground group would be taking the
// keyboard off whatever is really using it -- and tty_set_console_owner()
// moves both halves at once, so putting them back is not one call. What
// is checkable without touching anything is whether the three things
// `tty` reports still agree with the three subsystems that own them,
// which is exactly the drift a provider introduces.
#include "ktest.h"
#include "query.h"
#include "tty.h"
#include "keyboard.h"
#include "win_server.h"
#include "string.h"

KTEST("tty", "the console provider is registered and agrees with the tty layer") {
    struct query_tty t;
    int n = query_read(QUERY_TTY, 0, &t, sizeof t);
    KTEST_ASSERT_EQ(n, (int)sizeof t);
    KTEST_ASSERT_EQ((int)t.owner_pid, tty_console_owner());
    KTEST_ASSERT_EQ((int)t.foreground_pgid, tty_foreground_pgid());
    KTEST_ASSERT_EQ((int)t.compositor_pid, win_server_compositor_pid());
}

KTEST("tty", "the keyboard flags say WHICH reason, not just that there is one") {
    // api/keyboard.h keeps two flags rather than one boolean because
    // both reasons can hold at once, and whichever released second
    // would otherwise hand the keyboard back while the other still owns
    // it. That only means anything if the combined predicate really is
    // the OR of the two -- a report where SUSPENDED could be set with
    // neither reason would be describing a machine nobody could explain.
    struct query_tty t;
    KTEST_ASSERT_EQ(query_read(QUERY_TTY, 0, &t, sizeof t), (int)sizeof t);

    int either = (t.flags & (QUERY_TTY_COMPOSITOR | QUERY_TTY_CLAIMED)) != 0;
    int suspended = (t.flags & QUERY_TTY_SUSPENDED) != 0;
    KTEST_ASSERT_EQ(suspended, either);
    KTEST_ASSERT_EQ(suspended, keyboard_blocking_suspended() ? 1 : 0);
}

KTEST("tty", "an owned console always has a foreground group") {
    // kernel/tty.h's stated invariant: setting an owner puts that
    // owner's own group in front, so there is never a window where the
    // console has an owner and nothing to interrupt. A console with an
    // owner and no foreground group would make Ctrl-C silently do
    // nothing while everything else looked healthy.
    struct query_tty t;
    KTEST_ASSERT_EQ(query_read(QUERY_TTY, 0, &t, sizeof t), (int)sizeof t);
    if (t.owner_pid == 0) KTEST_SKIP("nobody owns the console on this boot");
    KTEST_ASSERT(t.foreground_pgid != 0);
}

// --- the line discipline ---------------------------------------------
//
// **THESE ARE THE TESTS THAT SAY THE ABSTRACTION IS REAL.** They create
// a terminal on a driver that is neither a keyboard nor a window -- it
// just records what was emitted -- and then assert on canonical mode,
// echo, erase and kill without a screen, a process or a keystroke
// anywhere in sight. If the discipline could only be tested through the
// physical console, it would not be a layer; it would still be the
// console with extra steps.
//
// A scratch terminal is created and destroyed inside each test, so a
// KTEST running in the live kernel neither disturbs tty0 nor leaks a
// slot into `/bin/tty`'s listing.

static char g_emit[256];
static unsigned g_emit_len;

static void test_output(struct tty *t, const char *buf, unsigned len) {
    (void)t;
    for (unsigned i = 0; i < len && g_emit_len < sizeof g_emit; i++)
        g_emit[g_emit_len++] = buf[i];
}

static const struct tty_driver test_driver = { .name = "test", .output = test_output };

static struct tty *scratch(uint32_t lflag) {
    g_emit_len = 0;
    struct tty *t = tty_create(&test_driver, 0);
    if (!t) return 0;
    struct tty_termios tio;
    tty_get_termios(t, &tio);
    tio.lflag = lflag;
    tty_set_termios(t, &tio);
    return t;
}

static void feed(struct tty *t, const char *s) {
    for (const char *p = s; *p; p++) tty_input(t, (uint8_t)*p, 0);
}

KTEST("tty", "raw mode makes a byte readable the moment it arrives") {
    struct tty *t = scratch(0);
    if (!t) KTEST_SKIP("no free terminal slot");
    feed(t, "hi");
    char buf[8] = {0};
    unsigned n = tty_read(t, buf, sizeof buf);
    int emitted = (int)g_emit_len;
    tty_destroy(t);

    KTEST_ASSERT_EQ((int)n, 2);
    KTEST_ASSERT_EQ(buf[0], 'h');
    KTEST_ASSERT_EQ(buf[1], 'i');
    // ECHO was off, so the terminal emitted nothing. Worth asserting:
    // the raw path and the echo path are separate branches, and a raw
    // mode that echoed anyway would double every character a shell
    // paints for itself.
    KTEST_ASSERT_EQ(emitted, 0);
}

KTEST("tty", "canonical mode returns NOTHING until the line ends") {
    struct tty *t = scratch(TTY_ICANON);
    if (!t) KTEST_SKIP("no free terminal slot");

    feed(t, "hello");
    char buf[16] = {0};
    // The load-bearing assertion: five characters have arrived and the
    // terminal is empty. A discipline that queued as it went would pass
    // every other check in this file and fail this one.
    unsigned early = tty_read(t, buf, sizeof buf);

    tty_input(t, '\n', 0);
    unsigned late = tty_read(t, buf, sizeof buf);
    tty_destroy(t);

    KTEST_ASSERT_EQ((int)early, 0);
    KTEST_ASSERT_EQ((int)late, 6); // "hello" and the newline
    KTEST_ASSERT_EQ(buf[5], '\n');
}

KTEST("tty", "a carriage return ends a line, and arrives as a newline") {
    // There is no iflag here, so ICRNL is unconditional -- ldisc.c says
    // why. A program reading a line must not have to handle two
    // spellings of "the line ended".
    struct tty *t = scratch(TTY_ICANON);
    if (!t) KTEST_SKIP("no free terminal slot");
    feed(t, "ok\r");
    char buf[8] = {0};
    unsigned n = tty_read(t, buf, sizeof buf);
    tty_destroy(t);

    KTEST_ASSERT_EQ((int)n, 3);
    KTEST_ASSERT_EQ(buf[2], '\n');
}

KTEST("tty", "erase rubs out a character, on the screen as well as in the line") {
    struct tty *t = scratch(TTY_ICANON | TTY_ECHO);
    if (!t) KTEST_SKIP("no free terminal slot");

    feed(t, "ab\b" "c\n");
    char buf[16] = {0};
    unsigned n = tty_read(t, buf, sizeof buf);
    // "ab" echoed, then the rub-out sequence, then "c" and the newline.
    // Asserted as well as the buffer because ECHO is the half a program
    // in canonical mode CANNOT do for itself -- it was never told a key
    // was pressed.
    int saw_rubout = 0;
    for (unsigned i = 0; i + 2 < g_emit_len; i++)
        if (g_emit[i] == '\b' && g_emit[i+1] == ' ' && g_emit[i+2] == '\b') saw_rubout = 1;
    tty_destroy(t);

    KTEST_ASSERT_EQ((int)n, 3);
    KTEST_ASSERT_EQ(buf[0], 'a');
    KTEST_ASSERT_EQ(buf[1], 'c');
    KTEST_ASSERT(saw_rubout);
}

KTEST("tty", "kill discards the whole pending line") {
    struct tty *t = scratch(TTY_ICANON);
    if (!t) KTEST_SKIP("no free terminal slot");

    feed(t, "rm -rf /");
    tty_input(t, 0x15, 0); // ^U
    feed(t, "ls\n");
    char buf[32] = {0};
    unsigned n = tty_read(t, buf, sizeof buf);
    tty_destroy(t);

    // Three, not eleven: the killed line must not reappear in front of
    // the one that replaced it.
    KTEST_ASSERT_EQ((int)n, 3);
    KTEST_ASSERT_EQ(buf[0], 'l');
}

KTEST("tty", "an over-long line is discarded, not truncated") {
    struct tty *t = scratch(TTY_ICANON);
    if (!t) KTEST_SKIP("no free terminal slot");

    // Past the buffer, then end the line. What must NOT happen is a
    // partial line arriving as if it had ended -- a program handed half
    // a command has no way to know it was half. Linux's N_TTY discards
    // the overflow for the same reason.
    for (int i = 0; i < 400; i++) tty_input(t, 'x', 0);
    tty_input(t, '\n', 0);

    // static, and cleared with k_memset(): a 512-byte `= {0}` local
    // makes GCC emit a call to `memset`, which this kernel does not
    // provide under that name -- and it is a link error rather than a
    // silent one, which is the good kind.
    static char buf[512];
    k_memset(buf, 0, sizeof buf);
    unsigned n = tty_read(t, buf, sizeof buf);
    tty_destroy(t);

    KTEST_ASSERT_EQ((int)n, 257); // the full buffer plus the newline
    KTEST_ASSERT_EQ(buf[256], '\n');
}

KTEST("tty", "INTR with nobody in front is delivered as an ordinary byte") {
    // The case that keeps Ctrl-C working at a prompt: with no job
    // running there is nothing to signal, so 0x03 goes through and the
    // line editor's KLINE_CANCEL abandons the line. A discipline that
    // swallowed it unconditionally would make Ctrl-C do nothing at an
    // empty prompt, which is exactly how this looked before the
    // foreground group existed.
    struct tty *t = scratch(TTY_ISIG);
    if (!t) KTEST_SKIP("no free terminal slot");
    tty_input(t, 0x03, 0);
    char buf[4] = {0};
    unsigned n = tty_read(t, buf, sizeof buf);
    tty_destroy(t);

    KTEST_ASSERT_EQ((int)n, 1);
    KTEST_ASSERT_EQ(buf[0], 0x03);
}

KTEST("tty", "a muted discipline passes bytes straight through") {
    // What a compositor holding the keyboard gets: no canonical
    // buffering, no echo onto a screen it does not own. KDSKBMODE/K_OFF
    // on a Linux VT.
    struct tty *t = scratch(TTY_ICANON | TTY_ECHO | TTY_ISIG);
    if (!t) KTEST_SKIP("no free terminal slot");
    tty_set_bypass(t, 1);
    feed(t, "ab");
    char buf[8] = {0};
    unsigned n = tty_read(t, buf, sizeof buf);
    int emitted = (int)g_emit_len;
    tty_destroy(t);

    KTEST_ASSERT_EQ((int)n, 2);   // readable at once despite ICANON
    KTEST_ASSERT_EQ(emitted, 0);  // and nothing painted despite ECHO
}

KTEST("tty", "changing the mode discards a half-typed line") {
    // POSIX's TCSAFLUSH. Carrying the line into raw mode would make its
    // bytes readable at the instant of the change -- which is not what
    // a program asking for raw mode asked for; it asked for what
    // arrives NEXT.
    struct tty *t = scratch(TTY_ICANON);
    if (!t) KTEST_SKIP("no free terminal slot");
    feed(t, "half");

    struct tty_termios tio;
    tty_get_termios(t, &tio);
    tio.lflag = 0; // raw
    tty_set_termios(t, &tio);

    char buf[16] = {0};
    unsigned n = tty_read(t, buf, sizeof buf);
    tty_destroy(t);
    KTEST_ASSERT_EQ((int)n, 0);
}

KTEST("tty", "every terminal has its own wait channel, and tty0 cannot be destroyed") {
    // The reason SCHED_CHAN_KEY had to go: one global "a key happened"
    // channel would wake every window's shell for a key typed at any of
    // them, which is the thundering herd the channel mechanism exists to
    // prevent (scheduler.h).
    struct tty *a = tty_create(&test_driver, 0);
    struct tty *b = tty_create(&test_driver, 0);
    if (!a || !b) {
        if (a) tty_destroy(a);
        if (b) tty_destroy(b);
        KTEST_SKIP("no free terminal slots");
    }
    const void *ca = tty_wait_chan(a), *cb = tty_wait_chan(b);
    const void *c0 = tty_wait_chan(tty_console());
    tty_destroy(a);
    tty_destroy(b);

    KTEST_ASSERT(ca != cb);
    KTEST_ASSERT(ca != c0 && cb != c0);

    // tty0 is not destroyable: the machine has one physical console and
    // a freed one would leave the keyboard IRQ feeding a dead object.
    tty_destroy(tty_console());
    KTEST_ASSERT(tty_at(0) == tty_console());
}
