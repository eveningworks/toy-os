// Signals and process groups, from ring 3 -- the syscall surface that
// kernel/proc/signal_test.c's KTESTs cannot reach.
//
// MUST BE SCHEDULER-SPAWNED, not driven by `run`. The legacy loader has
// no procs[] slot, so it has no pid, no group, no pending mask and no
// children -- every check here would be measuring the absence of a
// process rather than the behaviour of one. kernel/proc/signal_test.c
// spawns it, the same arrangement cputime_test and pipe_test already
// have, and tools/usertest_run.py excludes it by name with that reason.
//
// WHAT THIS IS FOR, beyond coverage: the kernel half can assert that a
// pending bit gets set, and cannot assert that a process actually DIES
// of it -- that needs a real process, really returning to ring 3, really
// being torn down, and a parent really reading the exit code back. That
// round trip is the thing worth testing, and it is only writable here.
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void check(const char *what, int ok, const char *detail) {
    char line[192];
    snprintf(line, sizeof line, "signal: %s %s%s%s\n", ok ? "ok  " : "FAIL",
             what, detail ? " -- " : "", detail ? detail : "");
    sys_eprint(line);
    if (!ok) fails++;
}

static void checkf(const char *what, int ok, int got, int want) {
    char d[64];
    snprintf(d, sizeof d, "got %d, wanted %d", got, want);
    check(what, ok, ok ? 0 : d);
}

// Spin until `pid` is really SCHED_BLOCKED, or give up. Returns 1 if it
// got there.
//
// A SPAWNED CHILD IS NOT RUNNING YET, and that difference is the whole
// point of this helper: a signal sent to a child that has not been
// scheduled once is delivered at its first syscall, which is a real and
// correct path but NOT the one a "does a blocked process get
// interrupted?" check is about.
//
// Yields rather than sleeping, so the child gets the CPU; bounded, so a
// child that never blocks fails the check instead of hanging the suite.
static int wait_until_blocked(int pid) {
    struct proc_info info;
    for (int spin = 0; spin < 20000; spin++) {
        for (int i = 0; sys_proc_info(i, &info); i++) {
            if (info.pid != pid) continue;
            if (info.state == PROC_STATE_BLOCKED) return 1;
            break;
        }
        sys_yield();
    }
    return 0;
}

// A child that will not exit on its own: spins across many timer slices.
// The point of a LONG-running one is that every check below is really
// about the signal rather than about the child having finished anyway --
// the failure mode this project keeps writing rules about.
#define SPINNER "/tests/spin_test"
#define SPIN_ARGS "100000"

int main(void) {
    int me = sys_getpid();

    // --- groups -------------------------------------------------------

    int mine = sys_getpgid(0);
    check("every process is in a group", mine > 0, 0);

    // Lead our own, so nothing below depends on which group whoever
    // spawned this test happens to be in.
    checkf("a process can lead its own group",
           sys_setpgid(PGID_SELF, PGID_SELF) == 0, sys_getpgid(0), me);
    checkf("...and getpgid reports it", sys_getpgid(0) == me, sys_getpgid(0), me);

    check("getpgid of a pid that does not exist is refused",
          sys_getpgid(4000) < 0, 0);

    // --- dispositions -------------------------------------------------

    // SIGKILL is what makes a force-quit trustworthy; a process that
    // could ignore it could not be ended at all.
    check("SIGKILL cannot be ignored", sys_sigaction(SIGKILL, SIG_IGN) < 0, 0);
    check("SIGQUIT cannot be ignored either", sys_sigaction(SIGQUIT, SIG_IGN) < 0, 0);
    // A HANDLER IS REFUSED rather than accepted and never called -- the
    // difference between a documented limit and a silent one.
    check("a handler pointer is refused, not accepted and ignored",
          sys_sigaction(SIGINT, 0x400000) < 0, 0);
    check("a signal number out of range is refused",
          sys_sigaction(99, SIG_IGN) < 0, 0);

    int prev = sys_sigaction(SIGINT, SIG_IGN);
    checkf("sigaction returns the PREVIOUS disposition", prev == SIG_DFL, prev, SIG_DFL);
    prev = sys_sigaction(SIGINT, SIG_IGN);
    checkf("...and reports it changed", prev == SIG_IGN, prev, SIG_IGN);

    // THE LOAD-BEARING ONE: an ignored signal must not kill us. If it
    // did, this program would simply stop and the harness would report
    // a truncated run rather than a failed check -- so the line after it
    // is the evidence.
    sys_kill(me, SIGINT);
    check("an IGNORED signal does not terminate the process", 1, 0);
    sys_sigaction(SIGINT, SIG_DFL);

    // SIGCHLD's default is to be ignored, which is what lets the kernel
    // send one on every child exit without every program knowing.
    sys_kill(me, SIGCHLD);
    check("SIGCHLD's default action is to be ignored", 1, 0);

    // --- a child dies of a signal, and says which one -------------------

    int child = sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, PGID_NEW);
    if (child <= 0) {
        check("spawned a long-running child", 0, "spawn failed");
        return fails ? 1 : 0;
    }
    checkf("a child can be spawned into a group of its OWN",
           sys_getpgid(child) == child, sys_getpgid(child), child);

    check("...which is not its parent's", sys_getpgid(child) != sys_getpgid(0), 0);

    sys_kill(child, SIGTERM);
    int code = -1;
    sys_waitpid(child, &code);
    // 128 + the signal, the convention every Unix shell prints -- and
    // the whole reason a signalled death is distinguishable from an
    // ordinary non-zero exit.
    checkf("a signalled child reports 128 + the signal",
           code == SIGNAL_EXIT_BASE + SIGTERM, code, SIGNAL_EXIT_BASE + SIGTERM);

    // --- a whole GROUP, with one call -----------------------------------
    //
    // The property process groups exist for: `cat | grep | less` must
    // die as a unit. Two children in one group, one kill.
    int a = sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, PGID_NEW);
    int b = a > 0 ? sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, sys_getpgid(a)) : -1;
    if (a > 0 && b > 0) {
        int pg = sys_getpgid(a);
        checkf("two children can share one group", sys_getpgid(b) == pg,
               sys_getpgid(b), pg);
        // A NEGATIVE pid is the group, POSIX's spelling.
        check("kill(-pgid) reports success", sys_kill(-pg, SIGTERM) > 0, 0);

        int ca = -1, cb = -1;
        sys_waitpid(a, &ca);
        sys_waitpid(b, &cb);
        checkf("...and BOTH members died of it",
               ca == SIGNAL_EXIT_BASE + SIGTERM && cb == SIGNAL_EXIT_BASE + SIGTERM,
               ca, SIGNAL_EXIT_BASE + SIGTERM);
    } else {
        check("spawned two children into one group", 0, "spawn failed");
    }

    // --- a BLOCKED child is interruptible --------------------------------
    //
    // The half that needed EINTR. A process asleep in a blocking syscall
    // is not on its way back to ring 3, and delivery only happens there
    // -- so without waking it, a signal to a blocked process would be a
    // bit set forever. Parked on an EMPTY PIPE this process holds the
    // write end of, so nothing but the signal can release it: a child
    // blocked on the console would be woken by any keystroke and the
    // check would pass for the wrong reason.
    int fds[2];
    if (sys_pipe(fds) == 1) {
        int saved = sys_dup(0);
        sys_dup2(fds[0], 0);
        int reader = sys_spawn_group("/tests/catin", 0, -1, 0, PGID_NEW);
        sys_dup2(saved, 0);
        sys_close(saved);
        sys_close(fds[0]);   // the child holds its own copy

        if (reader > 0) {
            // WAIT UNTIL IT IS REALLY PARKED, and assert that it got
            // there. Without this the check passed with the EINTR wake
            // disabled entirely: the child had not been scheduled yet,
            // so the signal landed on a READY process and was delivered
            // at its FIRST syscall -- a correct outcome, of a different
            // mechanism than the one under test. The fixture never
            // reached the code under test (CLAUDE.md), and only a
            // positive control showed it.
            int parked = wait_until_blocked(reader);
            check("...and it really is parked before the signal is sent", parked, 0);

            // Nothing is ever written, and this process keeps the write
            // end open -- so the child cannot see EOF either. It is
            // parked, and only a signal can end it.
            sys_kill(reader, SIGTERM);
            int rc = -1;
            sys_waitpid(reader, &rc);
            checkf("a child BLOCKED in a syscall is interrupted and dies",
                   parked && rc == SIGNAL_EXIT_BASE + SIGTERM,
                   rc, SIGNAL_EXIT_BASE + SIGTERM);
        } else {
            check("spawned a child that blocks on a pipe", 0, "spawn failed");
        }
        sys_close(fds[1]);
    } else {
        check("could get a pipe for the blocking check", 0, "no pipes");
    }

    // --- SIGKILL reaches a process that ignores everything ---------------
    int stubborn = sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, PGID_NEW);
    if (stubborn > 0) {
        sys_kill(stubborn, SIGKILL);
        int sc = -1;
        sys_waitpid(stubborn, &sc);
        checkf("SIGKILL terminates, and reports 128 + 9",
               sc == SIGNAL_EXIT_BASE + SIGKILL, sc, SIGNAL_EXIT_BASE + SIGKILL);
    }

    // --- signalling something that is not there ---------------------------
    check("a signal to a pid that does not exist is refused",
          sys_kill(4000, SIGTERM) <= 0, 0);
    check("a group with no members is refused", sys_kill(-4000, SIGTERM) <= 0, 0);
    check("a number that is not a signal is refused", sys_kill(me, 99) <= 0, 0);

    char sum[96];
    snprintf(sum, sizeof sum, "signal: %s\n", fails ? "FAILURES" : "all checks passed");
    sys_eprint(sum);
    return fails ? 1 : 0;
}
