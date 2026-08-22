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
