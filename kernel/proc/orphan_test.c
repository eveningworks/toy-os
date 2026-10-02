// POSIX's orphaned-process-group test, which decides whether a
// background terminal read is STOPPED or REFUSED. See
// scheduler_pgid_orphaned() and tty.c's tty_check_background_read().
//
// **WHAT IS NOT COVERED HERE, STATED RATHER THAN IMPLIED: the NOT
// orphaned case.** Every check below asserts a group IS orphaned, or
// that a pgid nobody occupies is not -- so a version of the predicate
// that answered "orphaned" for everything would pass all of them. The
// case that would catch it needs a parent outside the group in the SAME
// SESSION, and a fabricated slot gets a session of its own with no
// setter to change it; building one would mean test-only API, which is
// worse than saying so. The end-to-end version of that case is a
// background read being STOPPED rather than refused, and nothing in
// this tree tests that either -- see docs/roadmap.md's test-harness
// coverage item.
#include "ktest.h"
#include "scheduler.h"
#include "string.h"

// Two fabricated slots: a "child" and a "parent" whose relationship the
// checks below move around. scheduler_test_park() gives each its own
// pgid and sid, which is a process that leads its own group -- the
// starting point every case here changes one thing from.
struct pair { int c, p; uint64_t ctf[SCHED_TF_SLOTS], ptf[SCHED_TF_SLOTS]; };

static int pair_make(struct pair *x) {
    static const char chan;
    k_memset(x, 0, sizeof *x);
    x->c = scheduler_test_park(x->ctf, &chan, SCHED_WAIT_KEY);
    x->p = scheduler_test_park(x->ptf, &chan, SCHED_WAIT_KEY);
    return x->c >= 0 && x->p >= 0;
}
static void pair_free(struct pair *x) {
    if (x->c >= 0) scheduler_test_release(x->c);
    if (x->p >= 0) scheduler_test_release(x->p);
}

KTEST("orphan", "a group whose only parent is inside it is orphaned") {
    struct pair x;
    if (!pair_make(&x)) { pair_free(&x); KTEST_SKIP("no free process slots"); }
    int cpid = scheduler_slot_pid(x.c), ppid = scheduler_slot_pid(x.p);
    // Both in ONE group: the parent cannot rescue a group it is in.
    KTEST_ASSERT(scheduler_setpgid(ppid, ppid));
    KTEST_ASSERT(scheduler_setpgid(cpid, ppid));
    KTEST_ASSERT(scheduler_reparent(cpid, ppid));
    KTEST_ASSERT(scheduler_pgid_orphaned(ppid));
    pair_free(&x);
}

// AND THE SESSION CLAUSE IS NOT DECORATION. The same shape with the
// parent in a DIFFERENT session is orphaned again: a process elsewhere
// in the system has no claim on this terminal and would never think to
// continue the group. Constructed from the fabricator's defaults, which
// give every slot a session of its own.
KTEST("orphan", "a parent in another session does not rescue it") {
    struct pair x;
    if (!pair_make(&x)) { pair_free(&x); KTEST_SKIP("no free process slots"); }
    int cpid = scheduler_slot_pid(x.c), ppid = scheduler_slot_pid(x.p);
    KTEST_ASSERT(scheduler_setpgid(cpid, cpid));
    KTEST_ASSERT(scheduler_reparent(cpid, ppid));
    KTEST_ASSERT(scheduler_sid(cpid) != scheduler_sid(ppid));
    KTEST_ASSERT(scheduler_pgid_orphaned(cpid));
    pair_free(&x);
}

KTEST("orphan", "a group nobody is in is not orphaned -- it does not exist") {
    // The distinction matters: "orphaned" drives a read to EIO, and a
    // pgid with no members would otherwise refuse reads for a group
    // that was never there.
    KTEST_ASSERT(!scheduler_pgid_orphaned(SCHED_PID_MAX));
    KTEST_ASSERT(!scheduler_pgid_orphaned(0));
    KTEST_ASSERT(!scheduler_pgid_orphaned(-1));
}
