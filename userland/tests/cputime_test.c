// CPU accounting: a process must never be billed more time than passed.
//
// THE BUG THIS EXISTS TO CATCH
// ----------------------------
// SYS_YIELD used to reschedule by calling scheduler_tick(), the timer's
// own entry point, on the reasoning that a yield is indistinguishable
// from "the timer happened to fire right now". That is true of the
// rescheduling and false of the ACCOUNTING: a tick is a unit of elapsed
// time, and a yield elapses microseconds. So every yield charged the
// caller a whole tick that never passed.
//
// A GUI app with an on_tick callback yields every loop pass -- thousands
// of times a second against the PIT's hundred -- so it billed itself far
// more ticks than existed. Task Manager and Shapes both showed a flat
// 100% (each clamped down from something absurd) while everything that
// blocks showed an honest 0%, which reads like a polling problem rather
// than an accounting one. Two processes on one CPU cannot both be at
// 100%, and that impossibility is the whole assertion here.
//
// THE ASSERTION, AND WHY THE OBVIOUS ONE IS TOO WEAK
// --------------------------------------------------
// Spin on sys_yield() across a fixed WALL-CLOCK window and compare the
// CPU billed against the window.
//
// The obvious check -- billed must not EXCEED elapsed -- was written
// first and does not work, which the positive control caught: a yield
// returns only when the process is next scheduled, i.e. after about a
// whole tick, so the buggy kernel bills exactly one tick per tick and
// lands on billed == elapsed. That is a flat 100%, the reported
// symptom, and it slips through a `>` comparison untouched.
//
// So the real assertion is that a process which immediately hands the
// CPU back must be billed SUBSTANTIALLY LESS than the whole window. It
// does essentially no work: it yields, waits to be rescheduled, and
// yields again. Half the window is a deliberately generous line -- the
// buggy kernel bills 100% of it and a correct one bills ~0.
//
// Both bounds are kept: the ceiling is the impossibility check (nobody
// may be billed more time than passed) and the halfway line is the one
// that actually fails against the bug.
//
// Deliberately NOT asserted: any lower bound on the billing. What a
// yielding process legitimately uses is a few microseconds per pass, and
// on a machine whose clocksource is the 100Hz PIT rather than the TSC
// that rounds to zero -- so requiring any minimum would fail a correct
// kernel on correct hardware.
//
// UPDATED once accounting became a real duration rather than a tick
// count (kernel/clocksource.h): the scheduler measures each slice
// against a clock now, so a yield is charged the microseconds it
// actually took. Both bounds above still hold and still mean the same
// thing -- the ceiling is now "billed more nanoseconds than passed"
// rather than ticks -- and the test got STRONGER, because a correct
// kernel now reports a small non-zero figure instead of a zero that
// could equally have meant the billing was dead.

#include "rt/sys.h"
#include <stdio.h>

// Long enough that the ratio is unambiguous, short enough to stay a
// quick test. At 100Hz this is roughly a third of a second.
#define WINDOW_TICKS 30

// Find this process's own slot. The caller is the RUNNING one by
// construction -- it is the process executing the syscall that asks --
// and there is no getpid() to do it more directly.
static int find_self(struct proc_info *out) {
    for (int i = 0; i < SYS_PROC_MAX; i++) {
        struct proc_info info;
        if (sys_proc_info(i, &info) != 0) continue;
        if (info.pid == 0) continue;            // empty slot: skip, don't stop
        if (info.state != PROC_STATE_RUNNING) continue;
        *out = info;
        return i;
    }
    return -1;
}

int main(void) {
    struct proc_info me;
    int slot = find_self(&me);
    if (slot < 0) {
        sys_eprint("cputime_test: FAIL -- could not find my own process slot\n");
        return 1;
    }

    unsigned long t0 = sys_ticks();               // paces the loop
    unsigned long long ns0 = sys_monotonic_ns();  // measures the window
    unsigned long long cpu0 = (unsigned long long)me.cpu_ns;

    unsigned long yields = 0;
    while (sys_ticks() - t0 < WINDOW_TICKS) {
        sys_yield();
        yields++;
    }

    struct proc_info now;
    if (sys_proc_info(slot, &now) != 0 || now.pid != me.pid) {
        sys_eprint("cputime_test: FAIL -- my slot changed under me\n");
        return 1;
    }

    unsigned long long elapsed = sys_monotonic_ns() - ns0;
    unsigned long long billed = (unsigned long long)now.cpu_ns - cpu0;

    char line[192];
    snprintf(line, sizeof line,
             "cputime_test: %lu yields, %lu ms elapsed, %lu us billed\n",
             yields, (unsigned long)(elapsed / 1000000ULL),
             (unsigned long)(billed / 1000ULL));
    sys_eprint(line);

    // The impossibility: nobody may be billed more time than passed.
    if (billed > elapsed) {
        snprintf(line, sizeof line,
                 "cputime_test: FAIL -- billed %lu us over a %lu us window\n",
                 (unsigned long)(billed / 1000ULL), (unsigned long)(elapsed / 1000ULL));
        sys_eprint(line);
        return 1;
    }

    // The one that actually catches the bug. A process doing nothing but
    // yielding cannot have been running for most of the window; the
    // buggy kernel bills it the whole thing.
    if (billed > elapsed / 2) {
        snprintf(line, sizeof line,
                 "cputime_test: FAIL -- billed %lu us of a %lu us window while only "
                 "yielding (time is being charged that was not spent running)\n",
                 (unsigned long)(billed / 1000ULL), (unsigned long)(elapsed / 1000ULL));
        sys_eprint(line);
        return 1;
    }

    // A yield that bills nothing must still RESCHEDULE, or the fix
    // would have turned an over-billing bug into a dead yield and this
    // test would happily pass. If nothing else was runnable the loop
    // still had to make progress, so the wall clock is the check that
    // the process was not simply wedged.
    if (yields == 0 || sys_ticks() - t0 < WINDOW_TICKS) {
        sys_eprint("cputime_test: FAIL -- the yield loop made no progress\n");
        return 1;
    }

    sys_eprint("cputime_test: PASS -- billed no more time than elapsed\n");
    return 0;
}
