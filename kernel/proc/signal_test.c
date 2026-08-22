// Signals and process groups: the kernel-side state machine, plus the
// driver for the ring-3 half.
//
// THE SPLIT, and it is the same one cputime_test and pipe_test make:
// what can be asserted from ring 0 is that the BOOKKEEPING is right --
// a pending bit set, an ignored signal dropped, a group joined. What
// cannot is that a process actually dies of it, which needs a real
// process really returning to ring 3. userland/tests/signal_test.c is
// that half, and the last KTEST here spawns it.
//
// Every test that fabricates a process slot follows scheduler.h's two
// rules for scheduler_test_park(): hold the preemption guard across the
// whole window, and RELEASE BEFORE ASSERTING -- a KTEST_ASSERT returns
// from the body, so an assertion made while a slot is still fabricated
// leaks a READY slot with a fake trapframe, which is the exact crash
// rule 1 exists to prevent.
#include "ktest.h"
#include "scheduler.h"
#include "signal.h"
#include "signal_abi.h"
#include "ksignal.h"
#include "syscall_abi.h"
#include "errno.h"
#include "string.h"
#include "timer.h"
#include "fs.h"

// --- the name table (shared with ring 3, api/ksignal.h) ---------------

KTEST("signal", "a signal's name round-trips through the shared table") {
    KTEST_ASSERT(k_strcmp(signal_name(SIGINT), "INT") == 0);
    KTEST_ASSERT(k_strcmp(signal_name(SIGTERM), "TERM") == 0);
    // Never NULL, so no caller has to handle one.
    KTEST_ASSERT(k_strcmp(signal_name(1234), "?") == 0);

    KTEST_ASSERT_EQ(signal_from_name("TERM"), SIGTERM);
    KTEST_ASSERT_EQ(signal_from_name("SIGTERM"), SIGTERM);
    KTEST_ASSERT_EQ(signal_from_name("sigterm"), SIGTERM);
    KTEST_ASSERT_EQ(signal_from_name("9"), SIGKILL);
    // REJECTS rather than guessing -- the argument names something to
    // destroy, so a partial parse is the wrong kind of forgiving.
    KTEST_ASSERT_EQ(signal_from_name("TERMINATE"), 0);
    KTEST_ASSERT_EQ(signal_from_name("9x"), 0);
    KTEST_ASSERT_EQ(signal_from_name("0"), 0);
    KTEST_ASSERT_EQ(signal_from_name(""), 0);
}

// --- sending to nothing ------------------------------------------------

KTEST("signal", "a signal to a pid or group that does not exist is refused") {
    KTEST_ASSERT(!signal_send(4000, SIGTERM));
    KTEST_ASSERT(!signal_send(0, SIGTERM));
    KTEST_ASSERT_EQ(signal_send_group(4000, SIGTERM), 0);
    // A number that is not a signal, to a pid that might be.
    KTEST_ASSERT(!signal_send(1, 99));
    KTEST_ASSERT(!signal_send(1, 0));
}

// --- the pending mask, on a fabricated slot ----------------------------

KTEST("signal", "raising a signal sets a pending bit, and taking it clears the set") {
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    int before = (int)scheduler_signal_pending(pid);
    int raised = scheduler_signal_raise(pid, SIGTERM);
    uint32_t mask = scheduler_signal_pending(pid);
    int took = scheduler_signal_take(pid);
    uint32_t after = scheduler_signal_pending(pid);
    int took_again = scheduler_signal_take(pid);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(before, 0);
    KTEST_ASSERT(raised);
    KTEST_ASSERT_EQ((int)mask, 1 << SIGTERM);
    KTEST_ASSERT_EQ(took, SIGTERM);
    KTEST_ASSERT_EQ((int)after, 0);
    KTEST_ASSERT_EQ(took_again, 0);
}

KTEST("signal", "two of the same signal before delivery are ONE signal") {
    // A bitmask, not a queue -- which is what ordinary Unix signals do
    // too, and is why a burst of Ctrl-C does not queue up N deaths.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    scheduler_signal_raise(pid, SIGINT);
    scheduler_signal_raise(pid, SIGINT);
    uint32_t mask = scheduler_signal_pending(pid);
    int first = scheduler_signal_take(pid);
    int second = scheduler_signal_take(pid);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ((int)mask, 1 << SIGINT);
    KTEST_ASSERT_EQ(first, SIGINT);
    KTEST_ASSERT_EQ(second, 0);
}

KTEST("signal", "the LOWEST pending signal is the one taken") {
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    scheduler_signal_raise(pid, SIGTERM); // 15
    scheduler_signal_raise(pid, SIGINT);  // 2
    int took = scheduler_signal_take(pid);
    // The WHOLE set is cleared, because acting on one terminates the
    // process and the rest could never be acted on.
    uint32_t left = scheduler_signal_pending(pid);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(took, SIGINT);
    KTEST_ASSERT_EQ((int)left, 0);
}

KTEST("signal", "an IGNORED signal is dropped at arrival, not queued") {
    // The invariant everything else rests on: `pending != 0` means "this
    // process must die", with no policy lookup. That is only true because
    // an ignored signal never gets a bit.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    int was = scheduler_signal_set_ignored(pid, SIGINT, 1);
    int raised = scheduler_signal_raise(pid, SIGINT);
    uint32_t pending = scheduler_signal_pending(pid);
    // SIGKILL is unignorable even when asked for, so it still lands.
    scheduler_signal_set_ignored(pid, SIGKILL, 1);
    scheduler_signal_raise(pid, SIGKILL);
    uint32_t after_kill = scheduler_signal_pending(pid);

    scheduler_signal_take(pid);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(was, 0);
    // "Delivered", because it was -- the action was nothing.
    KTEST_ASSERT(raised);
    KTEST_ASSERT_EQ((int)pending, 0);
    KTEST_ASSERT_EQ((int)after_kill, 1 << SIGKILL);
}

KTEST("signal", "starting to ignore a signal drops one already pending") {
    // Otherwise a process that has just said "I do not want this" is
    // killed by one that arrived a moment earlier -- and with `pending`
    // meaning "must die", that is exactly what would happen.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    scheduler_signal_raise(pid, SIGINT);
    uint32_t before = scheduler_signal_pending(pid);
    scheduler_signal_set_ignored(pid, SIGINT, 1);
    uint32_t after = scheduler_signal_pending(pid);

    scheduler_signal_take(pid);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ((int)before, 1 << SIGINT);
    KTEST_ASSERT_EQ((int)after, 0);
}

KTEST("signal", "a BLOCKED process is woken with -EINTR so it can be delivered to") {
    // Delivery happens on the way back to ring 3, and a parked process
    // is not on its way anywhere. -EINTR rather than SYS_RETRY, which
    // libsys retries in a loop -- that would send the caller straight
    // back into the call the signal is trying to end.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    int state_before = scheduler_test_state(idx);
    scheduler_signal_raise(pid, SIGTERM);
    int state_after = scheduler_test_state(idx);
    int64_t rax = (int64_t)tf[SCHED_TF_RAX];

    scheduler_signal_take(pid);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(state_before, PROC_STATE_BLOCKED);
    KTEST_ASSERT_EQ(state_after, PROC_STATE_READY);
    KTEST_ASSERT_EQ((int)rax, -EINTR);
}

// --- process groups ----------------------------------------------------

KTEST("signal", "every live process is in a group, and nothing is in group 0") {
    // Non-zero for every slot is what lets `pgid == 0` mean "no such
    // process" throughout, with no separate liveness argument.
    int seen = 0;
    struct proc_info info;
    for (int i = 0; i < scheduler_max_procs(); i++) {
        if (!scheduler_proc_info(i, &info) || info.pid == 0) continue;
        if (info.state == PROC_STATE_ZOMBIE) continue;
        seen++;
        KTEST_ASSERT(scheduler_pgid(info.pid) > 0);
    }
    // init at least, on every ordinary boot.
    KTEST_ASSERT(seen >= 1);
}

KTEST("signal", "setpgid joins an EXISTING group, or leads a new one") {
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    int lead = scheduler_setpgid(pid, pid);      // lead its own
    int own  = scheduler_pgid(pid);
    // A group nothing is in is refused -- the check that stops a typo
    // parking a process where no signal will ever find it.
    int bogus = scheduler_setpgid(pid, 3999);
    int still = scheduler_pgid(pid);
    int live  = scheduler_pgid_live(pid);
    int dead  = scheduler_pgid_live(3999);
    int nobody = scheduler_setpgid(4000, 4000);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT(lead);
    KTEST_ASSERT_EQ(own, pid);
    KTEST_ASSERT(!bogus);
    KTEST_ASSERT_EQ(still, pid);
    KTEST_ASSERT(live);
    KTEST_ASSERT(!dead);
    KTEST_ASSERT(!nobody);
}

KTEST("signal", "a group signal reaches every member and nobody else") {
    // Two fabricated slots in one group and a third outside it. The
    // third is the control: without it, "signalled both" would pass
    // equally well against a send that signalled EVERYTHING.
    uint64_t tf_a[SCHED_TF_SLOTS], tf_b[SCHED_TF_SLOTS], tf_c[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int ia = scheduler_test_park(tf_a, SCHED_CHAN_KEY, SCHED_WAIT_KEY);
    int ib = ia >= 0 ? scheduler_test_park(tf_b, SCHED_CHAN_KEY, SCHED_WAIT_KEY) : -1;
    int ic = ib >= 0 ? scheduler_test_park(tf_c, SCHED_CHAN_KEY, SCHED_WAIT_KEY) : -1;
    if (ic < 0) {
        if (ib >= 0) scheduler_test_release(ib);
        if (ia >= 0) scheduler_test_release(ia);
        scheduler_preempt_enable();
        KTEST_SKIP("needs three free process slots to fabricate");
    }
    int a = ia + 1, b = ib + 1, c = ic + 1;

    scheduler_setpgid(a, a);
    scheduler_setpgid(b, a);   // joins a's group
    scheduler_setpgid(c, c);   // its own

    int reached = signal_send_group(a, SIGTERM);
    uint32_t pa = scheduler_signal_pending(a);
    uint32_t pb = scheduler_signal_pending(b);
    uint32_t pc = scheduler_signal_pending(c);

    scheduler_signal_take(a);
    scheduler_signal_take(b);
    scheduler_signal_take(c);
    scheduler_test_release(ic);
    scheduler_test_release(ib);
    scheduler_test_release(ia);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(reached, 2);
    KTEST_ASSERT_EQ((int)pa, 1 << SIGTERM);
    KTEST_ASSERT_EQ((int)pb, 1 << SIGTERM);
    KTEST_ASSERT_EQ((int)pc, 0);   // the control
}

// --- the ring-3 half ----------------------------------------------------

#define SIGNAL_TEST_PATH "/tests/signal_test"

// Generous: the child spawns several long-running children of its own
// and waits for each to be signalled, and delivery is up to one tick
// late by design. Still under a couple of seconds in practice.
#define TIMEOUT_TICKS 900

KTEST("signal", "a real process is terminated by a signal and reports 128 + it") {
    if (!fs_exists(SIGNAL_TEST_PATH)) KTEST_SKIP("no " SIGNAL_TEST_PATH " on this boot");

    // SPAWNED, not `run`: the legacy loader has no procs[] slot, so the
    // child would have no pid, no group and no pending mask -- every
    // check inside it would measure the absence of a process.
    uint64_t t0 = pit_ticks();
    int pid = scheduler_spawn(SIGNAL_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1, exited = 0;
    while (pit_ticks() - t0 < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // Non-zero means a check inside it failed -- it prints each one to
    // stderr, so `dmesg` says WHICH rather than only that one did.
    KTEST_ASSERT_EQ(code, 0);
}
