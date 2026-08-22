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
#include "scheduler.h"
#include "string.h"
#include "pty.h"
#include "fs.h"
#include "timer.h"

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

// --- pseudo-terminals -------------------------------------------------
//
// The kernel-side half. What can only be checked from RING 3 -- that a
// pty behaves like a terminal to a program holding fds, and that a 0x03
// written to a master reaches a spawned child as SIGINT -- is
// /tests/pty_test, driven below by its exit code.

#define PTY_TEST_PATH "/tests/pty_test"
#define PTY_TIMEOUT_TICKS 600 // 6s at 100Hz; the child it spawns spins

KTEST("tty", "a pty is a terminal, with both ends counted separately") {
    int idx = pty_create();
    if (idx < 0) KTEST_SKIP("no free pty");

    struct tty *t = pty_tty(idx);
    int is_tty = t != NULL;
    // Its DRIVER is what differs from the console; everything above the
    // driver is the same object, which is the claim of the whole layer.
    int named_pty = is_tty && tty_driver_name(t)[0] == 'p';
    int both_open = pty_master_open(idx) && pty_slave_open(idx);

    // Closing ONE end must not free the pty: a master has to be able to
    // drain what a dead shell already printed.
    pty_close_slave(idx);
    int still_there = pty_valid(idx);
    int slave_gone = !pty_slave_open(idx);
    pty_close_master(idx);
    int gone = !pty_valid(idx);

    KTEST_ASSERT(is_tty);
    KTEST_ASSERT(named_pty);
    KTEST_ASSERT(both_open);
    KTEST_ASSERT(still_there);
    KTEST_ASSERT(slave_gone);
    KTEST_ASSERT(gone);
}

KTEST("tty", "a pty read tells WOULD BLOCK from END OF FILE") {
    // The rule a shell is written against, and the one a terminal gets
    // wrong: -1 means wait, 0 means there will never be more. A pty that
    // reported 0 for an empty-but-live terminal would make every shell
    // in a window exit the moment it caught up with its input.
    int idx = pty_create();
    if (idx < 0) KTEST_SKIP("no free pty");

    char buf[8];
    int64_t empty_live = pty_master_read(idx, buf, sizeof buf);
    pty_close_slave(idx);
    int64_t empty_dead = pty_master_read(idx, buf, sizeof buf);
    pty_close_master(idx);

    KTEST_ASSERT_EQ((int)empty_live, -1); // would block
    KTEST_ASSERT_EQ((int)empty_dead, 0);  // end of file
}

KTEST("tty", "a slave write with no master is discarded and reported") {
    // pipe_write()'s answer, for the same reason: there is no SIGPIPE
    // here, so the write is dropped and says so rather than raising
    // something this kernel cannot deliver.
    int idx = pty_create();
    if (idx < 0) KTEST_SKIP("no free pty");
    pty_close_master(idx);
    int64_t n = pty_slave_write(idx, "x", 1);
    pty_close_slave(idx);
    KTEST_ASSERT_EQ((int)n, 0);
}

KTEST("tty", "a pty carries a line, and INTR through it, from ring 3") {
    if (!fs_exists(PTY_TEST_PATH)) KTEST_SKIP("no " PTY_TEST_PATH " on this boot");
    if (!fs_exists("/tests/spin_test")) KTEST_SKIP("no /tests/spin_test on this boot");

    int pid = scheduler_spawn(PTY_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < PTY_TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // 0 = every phase worked. See userland/tests/pty_test.c for what
    // each other code means; they are distinct so this reports WHICH
    // phase broke rather than only that something did.
    KTEST_ASSERT_EQ(code, 0);
}
