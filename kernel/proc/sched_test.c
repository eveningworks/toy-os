// Tests for the scheduler's rotation, in particular the one property
// the kernel-as-a-rotation-participant change exists to provide: KERNEL
// code keeps getting CPU while a ring-3 process is ready to run.
//
// Why this needed a KTEST rather than a shell command or a GUI test:
// the property is invisible from inside a process (a process cannot
// tell whether the kernel ran between its slices) and it's invisible in
// a screenshot. It's only observable from kernel code that is itself
// competing with a running process for the CPU -- which is exactly what
// a KTEST body is, since tests run inside the live booted kernel on the
// ordinary kernel context. `schedtest` can't stand in for it either:
// it's on debug_console.c's DBG_BLOCKED_CMDS list, so it can't be
// driven from the serial console that CI runs through at all.
#include "ktest.h"
#include "win_input.h"
#include "scheduler.h"
#include "tty.h"
#include "pipe.h"        // pipe_wait_chan() -- one channel per pipe
#include "win_input.h"  // win_input_wait_chan() -- the compositor's own channel
#include "proc_info.h"  // PROC_STATE_*
#include <stddef.h>
#include "timer.h"
#include "elf_run.h" // elf_run_from_fs() -- the legacy blocking path
#include "fs.h"
#include "paging.h"  // paging_kernel_leaf() -- the guard-page checks below
#include "kfmt.h"    // klog_printf -- the timeout report below
#include "process.h" // process_context_is_armed()

// The silent long-running spinner this test drives -- see
// userland/spin_test.c for why it has to be silent (this test's own
// report is parsed off the same serial console).
#define SPIN_PATH "/tests/spin_test"

// How many DISTINCT timer ticks the kernel must observe while the
// process is still running, to call the two genuinely interleaved.
//
// The pre-change behaviour scored exactly 0 here, not "a low number":
// the kernel context resumed only on a tick that found nothing READY,
// so kernel code got no CPU at all between the spawn and the process's
// exit and the loop below couldn't take a single sample. Any margin
// above 0 therefore separates the two behaviours; 3 is chosen to also
// rule out a one-off fluke (a single stolen tick around spawn/exit)
// without assuming a particular timer rate or emulation speed.
#define MIN_OBSERVED_TICKS 3

// Hard bound on the whole test, in 100Hz ticks (~5s). A process that
// never exits must fail this test, not hang the suite -- the loop below
// is a busy-wait by design (that's the point: it's competing for the
// CPU), so nothing else would ever break it out.
#define TIMEOUT_TICKS 500

KTEST("sched", "kernel context keeps running while a process is ready") {
    // A RAM-only boot has no /tests -- skip rather than fail, same
    // convention the filesystem tests use.
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    int pid = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(pid != 0);

    uint64_t start = pit_ticks();
    uint64_t last_seen = start;
    int observed_ticks = 0; // distinct ticks seen while it was RUNNING
    int exited = 0;
    int exit_code = -1;

    while (pit_ticks() - start < TIMEOUT_TICKS) {
        enum sched_poll_result r = scheduler_poll(pid, &exit_code);
        if (r == SCHED_POLL_EXITED) { exited = 1; break; }
        KTEST_ASSERT(r == SCHED_POLL_RUNNING); // never INVALID for a live pid

        // Count ticks rather than loop iterations: an iteration count
        // only proves this loop ran, while a CHANGE in the tick counter
        // proves time passed with the process still alive -- i.e. that
        // the kernel was scheduled back after the process had the CPU,
        // which is the actual claim.
        uint64_t now = pit_ticks();
        if (now != last_seen) {
            last_seen = now;
            observed_ticks++;
        }
    }

    KTEST_ASSERT(exited);                            // it must finish, not time out
    KTEST_ASSERT_EQ(exit_code, 0);
    KTEST_ASSERT(observed_ticks >= MIN_OBSERVED_TICKS);
}

// The rotation must not lose the ability to reap, and a slot must come
// back for reuse afterwards -- spawning MAX_PROCS-worth of processes in
// sequence would fail on the second one if scheduler_poll()'s reap
// stopped freeing slots once the kernel joined the rotation.
KTEST("sched", "a slot is reusable after the process is reaped") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    for (int round = 0; round < 2; round++) {
        int pid = scheduler_spawn(SPIN_PATH, 0);
        KTEST_ASSERT(pid != 0);

        uint64_t start = pit_ticks();
        int exit_code = -1;
        int exited = 0;
        while (pit_ticks() - start < TIMEOUT_TICKS) {
            if (scheduler_poll(pid, &exit_code) == SCHED_POLL_EXITED) { exited = 1; break; }
        }
        KTEST_ASSERT(exited);
        KTEST_ASSERT_EQ(exit_code, 0);

        // Reaped -- polling the same pid again must now report INVALID,
        // not a second EXITED, or a caller could reap one exit twice.
        KTEST_ASSERT(scheduler_poll(pid, &exit_code) == SCHED_POLL_INVALID);
    }
}

// A scheduler-managed process must survive a LEGACY blocking process
// running alongside it. Two distinct bugs made it not, and both
// presented as an unrelated page fault in the scheduled process at some
// later syscall -- which is why this is a test rather than a comment:
//
//   * scheduler_tick() would switch away from the legacy process and
//     back, resuming it under whatever CR3 the other process left
//     loaded. process_run_ring3() keeps no procs[] entry, so there is
//     nowhere to record its address space.
//   * process_run_ring3() never set RSP0, inheriting whatever the last
//     switch_to() left there -- so the legacy process's traps landed on
//     a SCHEDULED process's kernel stack and overwrote the trapframe it
//     was suspended on.
//
// Neither was reachable before ring-3 GUI clients could stay alive
// across a shell command; both are now one `run` away.
KTEST("sched", "a scheduled process survives a legacy process running alongside") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");
    if (!fs_exists("/tests/exit_test")) KTEST_SKIP("no /tests/exit_test on this boot");

    int pid = scheduler_spawn(SPIN_PATH, "8");
    KTEST_ASSERT(pid != 0);

    // Let it get going, so it is genuinely mid-flight rather than not
    // yet started when the legacy process runs.
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 20) { }
    int code = -1;
    KTEST_ASSERT(scheduler_poll(pid, &code) == SCHED_POLL_RUNNING);

    // The legacy path, start to finish, while the above is suspended.
    // exit_test returns 42, which also confirms the legacy process
    // itself still works.
    int legacy = elf_run_from_fs("/tests/exit_test", 0);
    KTEST_ASSERT_EQ(legacy, 42);

    // The scheduled process must now run to a CLEAN exit. Before the
    // fixes it faulted instead, and its exit code came back as the
    // crash sentinel.
    int exited = 0;
    start = pit_ticks();
    while (pit_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    if (!exited) {
        // Same reasoning as win_input_test.c's report_stuck(): "it did
        // not exit" names nothing, and this one only fails under a trap
        // gate, where the difference between starved, parked and left
        // unschedulable is the whole question.
        struct proc_info pi;
        for (int i = 0; i < SCHED_MAX_PROCS; i++) {
            if (scheduler_proc_info(i, &pi) == 1 && pi.pid == pid) {
                klog_printf("sched_test: pid %d stuck -- state=%u wait=%u "
                            "cpu_ns=%llu cur=%d preempt=%d armed=%d\n",
                            pid, pi.state, pi.wait_reason,
                            (unsigned long long)pi.cpu_ns,
                            scheduler_current_pid(), scheduler_preempt_depth(),
                            process_context_is_armed());
                break;
            }
        }
        scheduler_trace_dump();
    }
    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(code, 0);
}

// --- kernel stack guard pages ----------------------------------------
//
// These assert a property nothing else can observe and whose failure is
// SILENT: if scheduler_guard_pages_init() runs out of split tables (or
// is never called, or runs before paging_enforce_wx() and gets its work
// undone), every stack is back to being a plain array with the next
// slot's saved trapframe directly below it -- which is the bug this
// whole mechanism was built for, presenting as a #GP on iretq in a
// process that did nothing wrong.
//
// The check has to ask the PAGE TABLES. A guard page that is still
// mapped behaves identically to one that is not, right up until
// something overflows.
KTEST("sched", "every kernel stack has an unmapped guard page below it") {
    int mapped = 0;
    for (int i = 0; i < 4; i++) {   // a sample: the pattern is uniform
        uint64_t base = scheduler_kstack_base(i);
        KTEST_ASSERT(base != 0);
        // The guard is the page immediately below the stack's lowest
        // address, so this is the address an overflow reaches first.
        uint64_t guard = base - 4096;
        if (paging_kernel_leaf(guard) & 1 /* PRESENT */) mapped++;
        // ...and the stack itself must still be there, which is the
        // half that would fail if the unmapping were off by one page.
        KTEST_ASSERT(paging_kernel_leaf(base) & 1);
    }
    KTEST_ASSERT_EQ(mapped, 0);
}

KTEST("sched", "the guard page is BELOW the stack, not inside it") {
    // An off-by-one that unmapped the stack's own first page would pass
    // the test above only if it also broke the second assertion there --
    // so this states the geometry directly instead.
    uint64_t b0 = scheduler_kstack_base(0);
    uint64_t b1 = scheduler_kstack_base(1);
    KTEST_ASSERT(b1 > b0);
    // Slot 1's stack starts a whole stack plus a whole guard above
    // slot 0's, which is what leaves room for a guard between them.
    KTEST_ASSERT_EQ(b1 - b0, (uint64_t)scheduler_kstack_kib() * 1024 + 4096);
}

// --- wait channels ---------------------------------------------------
//
// The property these assert is SELECTIVITY: a wake reaches the channel
// it names and nothing else. Waiting used to be by category, so one
// client's event woke every process blocked in SYS_WAIT_EVENT and one
// pipe's write woke the readers of every other -- correct, because each
// re-checked and re-parked, but O(waiters) wakeups per event on the two
// busiest paths in the system.
//
// Note what a WEAKER version of this test would still pass: asserting
// only that A woke proves nothing, because the broken implementation
// woke everybody and therefore also woke A. The load-bearing assertion
// is that B is STILL BLOCKED.

KTEST("sched", "a wake reaches only the channel it names") {
    uint64_t tf_a[SCHED_TF_SLOTS] = {0}, tf_b[SCHED_TF_SLOTS] = {0};
    static const char chan_a, chan_b; // two distinct addresses

    // Preemption OFF for the whole fabricated window -- see
    // scheduler.h. Nothing may spawn into these slots, and nothing may
    // try to RUN one once the wake below marks it READY.
    scheduler_preempt_disable();

    int a = scheduler_test_park(tf_a, &chan_a, SCHED_WAIT_EVENT);
    int b = scheduler_test_park(tf_b, &chan_b, SCHED_WAIT_PIPE);

    // Everything is captured, then the slots are released, and only
    // then is anything asserted: KTEST_ASSERT returns from the body, so
    // asserting here would leak a READY slot with a stack-local
    // trapframe on the failure path.
    int woke_a = -1, woke_b = -1;
    int state_a = -1, state_b = -1, state_b_after = -1;
    int64_t rax_a = -1, rax_b = -1, rax_b_after = -1;

    if (a >= 0 && b >= 0) {
        woke_a      = scheduler_wake(&chan_a, 4242);
        state_a     = scheduler_test_state(a);
        state_b     = scheduler_test_state(b);
        rax_a       = (int64_t)tf_a[SCHED_TF_RAX];
        rax_b       = (int64_t)tf_b[SCHED_TF_RAX];

        woke_b      = scheduler_wake(&chan_b, 7);
        state_b_after = scheduler_test_state(b);
        rax_b_after   = (int64_t)tf_b[SCHED_TF_RAX];
    }

    scheduler_test_release(a);
    scheduler_test_release(b);
    scheduler_preempt_enable();

    if (a < 0 || b < 0) KTEST_SKIP("no free process slots to fabricate");

    KTEST_ASSERT_EQ(woke_a, 1);                      // exactly one, not both
    KTEST_ASSERT_EQ(state_a, PROC_STATE_READY);      // A woke
    KTEST_ASSERT_EQ(state_b, PROC_STATE_BLOCKED);    // <- THE POINT: B did not
    KTEST_ASSERT_EQ(rax_a, 4242);                    // and A was answered
    KTEST_ASSERT_EQ(rax_b, 0);                       // and B was not

    KTEST_ASSERT_EQ(woke_b, 1);
    KTEST_ASSERT_EQ(state_b_after, PROC_STATE_READY);
    KTEST_ASSERT_EQ(rax_b_after, 7);
}

KTEST("sched", "a wake preempts only when it OUTRANKS what is running") {
    // The kernel context runs this test at the default level, 0. A wake
    // at a better level must ask for a switch on the way out of the
    // trap; one at the same level must not, or every interrupt would
    // rotate the CPU.
    uint64_t tf_hi[SCHED_TF_SLOTS] = {0}, tf_eq[SCHED_TF_SLOTS] = {0};
    static const char chan_hi, chan_eq;

    scheduler_preempt_disable();
    (void)scheduler_test_take_resched();

    int hi = scheduler_test_park(tf_hi, &chan_hi, SCHED_WAIT_EVENT);
    int eq = scheduler_test_park(tf_eq, &chan_eq, SCHED_WAIT_EVENT);
    int asked_eq = -1, asked_hi = -1, cur = scheduler_current_pid();

    if (hi >= 0 && eq >= 0) {
        scheduler_set_priority(hi + 1, -5);
        scheduler_wake(&chan_eq, 0);
        asked_eq = scheduler_test_take_resched();
        scheduler_wake(&chan_hi, 0);
        asked_hi = scheduler_test_take_resched();
    }

    scheduler_test_release(hi);
    scheduler_test_release(eq);
    scheduler_preempt_enable();

    if (hi < 0 || eq < 0) KTEST_SKIP("no free process slots to fabricate");
    if (cur) KTEST_SKIP("not run from the kernel context");

    KTEST_ASSERT_EQ(asked_eq, 0);
    KTEST_ASSERT_EQ(asked_hi, 1);
}

KTEST("sched", "a PREEMPTED process runs next at its level, not the one after it") {
    // Two READY slots at a level nothing real uses. The plain scan from
    // A's position picks B; A preempted must pick A -- or every wake of a
    // better-level driver resets the round-robin to just past it and a
    // slot scanned late is starved.
    uint64_t tf_a[SCHED_TF_SLOTS] = {0}, tf_b[SCHED_TF_SLOTS] = {0};
    static const char chan_a, chan_b;

    scheduler_preempt_disable();
    int a = scheduler_test_park(tf_a, &chan_a, SCHED_WAIT_EVENT);
    int b = scheduler_test_park(tf_b, &chan_b, SCHED_WAIT_EVENT);
    int plain = -2, head = -2, after = -2;

    if (a >= 0 && b >= 0) {
        scheduler_set_priority(a + 1, -17);
        scheduler_set_priority(b + 1, -17);
        scheduler_wake(&chan_a, 0);
        scheduler_wake(&chan_b, 0);
        (void)scheduler_test_take_resched();
        plain = scheduler_test_pick(a, -1);
        head  = scheduler_test_pick(a, a);
        after = scheduler_test_pick(a, -1);   // a head is served ONCE
    }

    scheduler_test_release(a);
    scheduler_test_release(b);
    scheduler_preempt_enable();

    if (a < 0 || b < 0) KTEST_SKIP("no free process slots to fabricate");

    KTEST_ASSERT_EQ(plain, b);
    KTEST_ASSERT_EQ(head, a);
    KTEST_ASSERT_EQ(after, b);
}

KTEST("sched", "a wake on a channel nobody holds wakes nothing") {
    static const char lonely;
    // 0 is a normal answer, not an error: a waker cannot know whether
    // anybody happened to be parked.
    KTEST_ASSERT_EQ(scheduler_wake(&lonely, 1), 0);
}

KTEST("sched", "every waitable object has its own channel") {
    // Selectivity above is only worth anything if the channels actually
    // differ -- a per-object wake whose objects share an address is the
    // category wake again, wearing a pointer.
    KTEST_ASSERT(scheduler_wait_chan_pid(1) != scheduler_wait_chan_pid(2));
    KTEST_ASSERT(win_input_wait_chan() != scheduler_wait_chan_pid(1));
    // The console terminal's channel, rather than a global "a key
    // happened" one -- there is no such thing now that a terminal is an
    // object, and a per-terminal channel is what stops a keystroke in
    // one window waking every other window's shell.
    KTEST_ASSERT(tty_wait_chan(tty_console()) != SCHED_CHAN_TIMER);
    KTEST_ASSERT(tty_wait_chan(tty_console()) != scheduler_wait_chan_pid(1));

    int a = pipe_create(), b = pipe_create();
    if (a >= 0 && b >= 0) {
        KTEST_ASSERT(pipe_wait_chan(a) != pipe_wait_chan(b));
        KTEST_ASSERT(pipe_wait_chan(a) != 0);
    }
    if (a >= 0) { pipe_close_reader(a); pipe_close_writer(a); }
    if (b >= 0) { pipe_close_reader(b); pipe_close_writer(b); }

    // A bad pid has no channel, rather than aliasing slot 0's.
    KTEST_ASSERT_EQ(scheduler_wait_chan_pid(0), NULL);
    KTEST_ASSERT_EQ(scheduler_wait_chan_pid(99999), NULL);
}
