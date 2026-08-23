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

// The channel these fabricated slots park on. Its ADDRESS is all that
// matters (scheduler.h), and it is this file's own rather than a real
// subsystem's: these tests are about signal delivery to a blocked
// process, not about what it is blocked ON, and borrowing a terminal's
// channel would make them fail the day something really woke it.
static const char park_chan;

// --- the name table (shared with ring 3, api/ksignal.h) ---------------

// The ignore bitmask these tests were written against became an action
// table when handlers landed (api/scheduler.h). This is the one-line
// bridge, kept rather than rewriting nine call sites: what they are
// each testing is the DROP-ON-ARRIVAL rule, not the shape of the state
// that records it.
static int test_set_ignored(int pid, int sig, int on) {
    struct sigaction act = { .handler = on ? SIG_IGN : SIG_DFL }, old;
    if (scheduler_signal_set_action(pid, sig, &act, &old) < 0) return -1;
    return old.handler == SIG_IGN;
}

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
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
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
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
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
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    scheduler_signal_raise(pid, SIGTERM); // 15
    scheduler_signal_raise(pid, SIGINT);  // 2
    int took = scheduler_signal_take(pid);
    // JUST THE ONE TAKEN. This asserted an empty set until handlers
    // landed, on the reasoning that acting on any signal terminated the
    // process so the rest could never be acted on -- true then, and
    // false the moment a handler can run and return. The SIGTERM is
    // still there, and it is delivered at the sigreturn that ends
    // SIGINT's handler.
    uint32_t left = scheduler_signal_pending(pid);
    int next = scheduler_signal_take(pid);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(took, SIGINT);
    KTEST_ASSERT_EQ((int)left, 1 << SIGTERM);
    KTEST_ASSERT_EQ(next, SIGTERM);
}

KTEST("signal", "an IGNORED signal is dropped at arrival, not queued") {
    // The invariant everything else rests on: `pending != 0` means "this
    // process must die", with no policy lookup. That is only true because
    // an ignored signal never gets a bit.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    int was = test_set_ignored(pid, SIGINT, 1);
    int raised = scheduler_signal_raise(pid, SIGINT);
    uint32_t pending = scheduler_signal_pending(pid);
    // SIGKILL is unignorable even when asked for, so it still lands.
    test_set_ignored(pid, SIGKILL, 1);
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
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    scheduler_signal_raise(pid, SIGINT);
    uint32_t before = scheduler_signal_pending(pid);
    test_set_ignored(pid, SIGINT, 1);
    uint32_t after = scheduler_signal_pending(pid);

    scheduler_signal_take(pid);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ((int)before, 1 << SIGINT);
    KTEST_ASSERT_EQ((int)after, 0);
}

KTEST("signal", "a BLOCKED process is woken by REWINDING its syscall") {
    // Delivery happens on the way back to ring 3, and a parked process
    // is not on its way anywhere. It used to be woken with -EINTR in its
    // RAX; it is rewound over its own `int $0x80` instead, so it
    // re-enters the kernel at a delivery point with the call NOT YET RUN
    // -- which is the only state in which SA_RESTART can exist at all
    // (kernel/proc/scheduler.c has the full argument).
    //
    // **THE VECTOR IS WHAT MAKES THIS TEST REACH THE CODE.** The
    // fabricated frame has to say it came from a syscall, because the
    // wake checks that before rewinding anything -- and the version of
    // this that did not set it passed while exercising the fallback
    // instead, which is the trap CLAUDE.md names.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    tf[SCHED_TF_VECTOR] = 0x80;      // "this frame is a syscall"
    tf[SCHED_TF_RIP]    = 0x400000;  // whatever follows the `int`
    tf[SCHED_TF_RAX]    = SYS_READ;  // the number a restart re-uses

    int state_before = scheduler_test_state(idx);
    scheduler_signal_raise(pid, SIGTERM);
    int state_after = scheduler_test_state(idx);
    uint64_t rip = tf[SCHED_TF_RIP];
    uint64_t rax = tf[SCHED_TF_RAX];

    scheduler_signal_take(pid);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(state_before, PROC_STATE_BLOCKED);
    KTEST_ASSERT_EQ(state_after, PROC_STATE_READY);
    KTEST_ASSERT_EQ((int)(0x400000 - rip), SYSCALL_INSN_LEN);
    // AND THE ARGUMENTS SURVIVE, which is the half a rewind is for: a
    // restart re-runs the call, and it can only do that if the number
    // and the argument registers are still the ones the caller passed.
    KTEST_ASSERT_EQ((int)rax, SYS_READ);
}

// --- job control: stop and continue -------------------------------------

KTEST("signal", "SIGSTOP suspends a process and SIGCONT resumes it") {
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    // Woken first, so the slot is READY rather than BLOCKED -- this test
    // is about the flag, and the blocked case is the next one.
    scheduler_wake(&park_chan, 0);
    int before = scheduler_test_state(idx);
    int sent   = signal_send(pid, SIGSTOP);
    int during = scheduler_test_state(idx);
    int was_stopped = scheduler_stopped(pid);
    int resumed = signal_send(pid, SIGCONT);
    int after   = scheduler_test_state(idx);
    int now_stopped = scheduler_stopped(pid);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT(sent);
    KTEST_ASSERT(resumed);
    KTEST_ASSERT_EQ(before, PROC_STATE_READY);
    KTEST_ASSERT_EQ(during, PROC_STATE_STOPPED);
    KTEST_ASSERT(was_stopped);
    KTEST_ASSERT_EQ(after, PROC_STATE_READY);
    KTEST_ASSERT(!now_stopped);
}

KTEST("signal", "a BLOCKED process stops without losing what it waits on") {
    // The whole argument for a flag rather than a fifth state: a
    // process suspended mid-syscall must come back to that syscall. It
    // stays parked on its channel while stopped, its wake still lands,
    // and continuing it leaves it exactly where the wake put it.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    signal_send(pid, SIGSTOP);
    int while_blocked = scheduler_test_state(idx);

    // The wake it was waiting for arrives WHILE it is stopped. It must
    // still be answered -- the trapframe gets its return value -- and
    // the process must still not run.
    int woken = scheduler_wake(&park_chan, 42);
    int after_wake = scheduler_test_state(idx);
    int64_t rax = (int64_t)tf[SCHED_TF_RAX];

    signal_send(pid, SIGCONT);
    int after_cont = scheduler_test_state(idx);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    // STOPPED outranks BLOCKED in the report: the block is no longer
    // why it is not running.
    KTEST_ASSERT_EQ(while_blocked, PROC_STATE_STOPPED);
    KTEST_ASSERT_EQ(woken, 1);
    KTEST_ASSERT_EQ(after_wake, PROC_STATE_STOPPED);
    KTEST_ASSERT_EQ((int)rax, 42);
    KTEST_ASSERT_EQ(after_cont, PROC_STATE_READY);
}

KTEST("signal", "a stop is reported to a waiter ONCE per suspension") {
    // POSIX's WUNTRACED rule, and the thing that stops a shell's
    // waitpid loop being handed the same suspension forever.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    signal_send(pid, SIGTSTP);
    int first  = scheduler_stop_report(pid);
    int second = scheduler_stop_report(pid);
    // A SECOND STOP WHILE ALREADY STOPPED IS NOT A SECOND EVENT.
    signal_send(pid, SIGTSTP);
    int third  = scheduler_stop_report(pid);
    // Continued and stopped again IS.
    signal_send(pid, SIGCONT);
    signal_send(pid, SIGSTOP);
    int fourth = scheduler_stop_report(pid);

    signal_send(pid, SIGCONT);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(first, SIGTSTP);
    KTEST_ASSERT_EQ(second, 0);
    KTEST_ASSERT_EQ(third, 0);
    KTEST_ASSERT_EQ(fourth, SIGSTOP);
}

KTEST("signal", "SIGSTOP cannot be ignored, and SIGCONT to a running process is a no-op") {
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    // SIGSTOP is unignorable for the reason SIGKILL is: there has to be
    // something that always works.
    test_set_ignored(pid, SIGSTOP, 1);
    signal_send(pid, SIGSTOP);
    int stopped_anyway = scheduler_stopped(pid);
    signal_send(pid, SIGCONT);

    // SIGTSTP, by contrast, IS ignorable -- that is the difference
    // between the key and the command, exactly as on Unix.
    test_set_ignored(pid, SIGTSTP, 1);
    signal_send(pid, SIGTSTP);
    int ignored_tstp = scheduler_stopped(pid);

    // Continuing something that was never stopped reports delivered and
    // changes nothing.
    int cont_ok = signal_send(pid, SIGCONT);
    int still_running = !scheduler_stopped(pid);

    test_set_ignored(pid, SIGSTOP, 0);
    test_set_ignored(pid, SIGTSTP, 0);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT(stopped_anyway);
    KTEST_ASSERT(!ignored_tstp);
    KTEST_ASSERT(cont_ok);
    KTEST_ASSERT(still_running);
}

KTEST("signal", "a stopped process is still killable") {
    // The reason SIGKILL is exempt from suspension everywhere: a job
    // suspended by Ctrl-Z must not be unkillable until somebody resumes
    // it. Fabricated slots cannot be scheduler_kill()'d (no address
    // space), so this asserts the state the kill path depends on --
    // that a stopped slot is still READY or BLOCKED underneath, which
    // is what scheduler_kill() requires.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    signal_send(pid, SIGSTOP);
    int alive = scheduler_pid_alive(pid);
    // A SIGTERM to a stopped process sets its bit and waits -- POSIX's
    // behaviour, and the one surprise worth a test: it is pending, and
    // it will not act until something continues the process.
    signal_send(pid, SIGTERM);
    int pending = scheduler_signal_pending(pid) != 0;
    int still_stopped = scheduler_stopped(pid);

    scheduler_signal_take(pid);
    signal_send(pid, SIGCONT);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT(alive);
    KTEST_ASSERT(pending);
    KTEST_ASSERT(still_stopped);
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
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
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
    int ia = scheduler_test_park(tf_a, &park_chan, SCHED_WAIT_KEY);
    int ib = ia >= 0 ? scheduler_test_park(tf_b, &park_chan, SCHED_WAIT_KEY) : -1;
    int ic = ib >= 0 ? scheduler_test_park(tf_c, &park_chan, SCHED_WAIT_KEY) : -1;
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

// --- handlers: the state rules a ring-3 test cannot reach ---------------

KTEST("signal", "SIGKILL and SIGSTOP cannot be BLOCKED, whatever the mask says") {
    // The rule exists because sigreturn restores this mask from a struct
    // on the USER STACK. A program that corrupts its own frame -- or
    // edits it on purpose -- must not be able to make itself unkillable,
    // so the clamp lives in the setter rather than at its callers.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    scheduler_signal_set_blocked(pid, 0xFFFFFFFFu); // block everything
    uint32_t got = scheduler_signal_blocked(pid);

    // AND THE CONSEQUENCE, not just the field: a blocked-everything
    // process must still have SIGKILL come out of the deliverable end.
    scheduler_signal_raise(pid, SIGINT);   // blocked -- must not surface
    int while_blocked = scheduler_signal_deliverable(pid);
    scheduler_signal_raise(pid, SIGKILL);  // must surface anyway
    int unblockable = scheduler_signal_deliverable(pid);

    scheduler_signal_set_blocked(pid, 0);
    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ((int)(got & (1u << SIGKILL)), 0);
    KTEST_ASSERT_EQ((int)(got & (1u << SIGSTOP)), 0);
    KTEST_ASSERT_EQ((int)(got & (1u << SIGINT)), 1 << SIGINT); // the control
    KTEST_ASSERT_EQ(while_blocked, 0);
    KTEST_ASSERT_EQ(unblockable, SIGKILL);
}

KTEST("signal", "a BLOCKED signal stays pending rather than being dropped") {
    // The distinction the whole mask rests on. Dropping would be simpler
    // and would make a handler that re-raises its own signal silently
    // lose it; POSIX defers, and so does this.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    scheduler_signal_set_blocked(pid, 1u << SIGINT);
    scheduler_signal_raise(pid, SIGINT);
    uint32_t pending_while_blocked = scheduler_signal_pending(pid);
    int deliverable_while_blocked  = scheduler_signal_deliverable(pid);

    scheduler_signal_set_blocked(pid, 0); // what sigreturn does
    int after_unblock = scheduler_signal_take(pid);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ((int)pending_while_blocked, 1 << SIGINT);
    KTEST_ASSERT_EQ(deliverable_while_blocked, 0);
    KTEST_ASSERT_EQ(after_unblock, SIGINT);
}

KTEST("signal", "installing a HANDLER keeps a pending signal; SIG_IGN drops it") {
    // Two rules that look like one and are not. SIG_IGN drops what is
    // already pending, because a process that has just said "I do not
    // want this" must not be acted on by one that arrived a moment
    // earlier. A HANDLER must NOT drop it -- there the pending signal is
    // precisely what the caller has just arranged to hear about.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    struct sigaction handler = { .handler = 0x400000, .restorer = 0x400010 };
    struct sigaction ignore  = { .handler = SIG_IGN };
    struct sigaction old;

    scheduler_signal_raise(pid, SIGINT);
    scheduler_signal_set_action(pid, SIGINT, &handler, &old);
    uint32_t kept = scheduler_signal_pending(pid);

    scheduler_signal_set_action(pid, SIGINT, &ignore, &old);
    uint32_t dropped = scheduler_signal_pending(pid);
    // And the readback names the handler that WAS there, not the one
    // being installed -- a previous-value report that reports the new
    // value is a call nobody can use to save and restore.
    uint64_t reported = old.handler;

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ((int)kept, 1 << SIGINT);
    KTEST_ASSERT_EQ((int)dropped, 0);
    KTEST_ASSERT_EQ((int)(reported == 0x400000), 1);
}

KTEST("signal", "a sentinel disposition reports back with no restorer left on it") {
    // Normalised on the way IN, so a readback cannot show SIG_DFL
    // carrying a stale restorer -- which would look armed and is not.
    uint64_t tf[SCHED_TF_SLOTS];
    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &park_chan, SCHED_WAIT_KEY);
    if (idx < 0) {
        scheduler_preempt_enable();
        KTEST_SKIP("no free process slot to fabricate");
    }
    int pid = idx + 1;

    struct sigaction handler = { .handler = 0x400000, .restorer = 0x400010,
                                 .flags = SA_RESTART };
    struct sigaction back_to_dfl = { .handler = SIG_DFL, .restorer = 0x400010,
                                     .flags = SA_RESTART };
    struct sigaction old, now;
    scheduler_signal_set_action(pid, SIGINT, &handler, &old);
    scheduler_signal_set_action(pid, SIGINT, &back_to_dfl, &old);
    scheduler_signal_action(pid, SIGINT, &now);

    scheduler_test_release(idx);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ((int)now.handler, SIG_DFL);
    KTEST_ASSERT_EQ((int)now.restorer, 0);
    KTEST_ASSERT_EQ((int)now.flags, 0);
}

// --- the ring-3 half ----------------------------------------------------

#define SIGNAL_TEST_PATH "/tests/signal_test"
// A silent, long-running spinner -- see userland/tests/spin_test.c for
// why it prints nothing. The stop test below needs a process that makes
// MEASURABLE progress, which is the one thing a fabricated slot cannot
// do.
#define SPIN_PATH "/tests/spin_test"

// Generous: the child spawns several long-running children of its own
// and waits for each to be signalled, and delivery is up to one tick
// late by design. Still under a couple of seconds in practice.
#define TIMEOUT_TICKS 900

// THE TEST THE BOOKKEEPING ONES CANNOT BE. Every stop test above
// asserts on scheduler_test_state(), which reads the flag -- so all of
// them stay green with find_next_runnable()'s stopped check deleted,
// and a "stopped" process that carries on running looks perfect. What
// actually has to be true is that a suspended process STOPS MAKING
// PROGRESS, and the only way to see that is to give a real one work to
// do and watch the CPU time it accrues.
//
// cpu_ns is the measurement because it is billed from the timer tick
// against whoever was running: a process nothing schedules cannot
// accumulate any, no matter what the process table says about it.
KTEST("signal", "a STOPPED process stops accruing CPU time, and resumes accruing it") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    // The long form: this has to outlive four observation windows.
    int pid = scheduler_spawn(SPIN_PATH, "600");
    KTEST_ASSERT(pid != 0);

    struct proc_info info;
    int slot = pid - 1;

    // Let it run first, so the measurement has something to compare
    // against -- a process that never got the CPU at all would show a
    // flat cpu_ns for reasons having nothing to do with the stop.
    uint64_t t0 = pit_ticks();
    while (pit_ticks() - t0 < 20) { }
    KTEST_ASSERT(scheduler_proc_info(slot, &info));
    uint64_t ran_before = info.cpu_ns;

    signal_send(pid, SIGSTOP);
    // Sampled AFTER the stop rather than straddling it: the tick that
    // suspends it may still bill the slice it was in the middle of.
    KTEST_ASSERT(scheduler_proc_info(slot, &info));
    uint64_t stopped_at = info.cpu_ns;
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 20) { }
    KTEST_ASSERT(scheduler_proc_info(slot, &info));
    uint64_t while_stopped = info.cpu_ns;

    signal_send(pid, SIGCONT);
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 20) { }
    KTEST_ASSERT(scheduler_proc_info(slot, &info));
    uint64_t after_cont = info.cpu_ns;

    int code = 0;
    scheduler_kill(pid, 0);
    scheduler_poll(pid, &code);

    KTEST_ASSERT(ran_before > 0);               // it really was running
    KTEST_ASSERT_EQ(while_stopped, stopped_at); // it really stopped
    KTEST_ASSERT(after_cont > while_stopped);   // it really resumed
}

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
