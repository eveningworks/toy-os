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
#include "syscall_abi.h" // SYS_RETRY -- what an entry-parked waiter is answered
#include <stddef.h>
#include "timer.h"
#include "elf_run.h" // elf_run_from_fs() -- the legacy blocking path
#include "fs.h"
#include "paging.h"  // paging_kernel_leaf() -- the guard-page checks below
#include "kfmt.h"    // klog_printf -- the timeout report below
#include "process.h" // process_context_is_armed()
#include "signal_abi.h" // SIGKILL -- a deferred kill is a pending one

// sched_internal.h's SCHED_SLOT_CHUNK, which this file cannot include.
#define SCHED_SLOT_CHUNK_TEST 64

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

// Hard bound on the whole test, in 100Hz ticks (~30s). A process that
// never exits must fail this test, not hang the suite -- the loop below
// is a busy-wait by design (that's the point: it's competing for the
// CPU), so nothing else would ever break it out.
//
// A HANG GUARD, NOT A SPEED LIMIT: spin_test takes 3.5-4 s under TCG on
// the development host, so the 5 s this used to be failed on GitHub's
// slower runner (2 of 2 runs of the v0.4.0 tag) and now and then here.
#define TIMEOUT_TICKS 3000

KTEST("sched", "kernel context keeps running while a process is ready") {
    // A RAM-only boot has no /tests -- skip rather than fail, same
    // convention the filesystem tests use.
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    int pid = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(pid != 0);

    uint64_t start = coarse_ticks();
    uint64_t last_seen = start;
    int observed_ticks = 0; // distinct ticks seen while it was RUNNING
    int exited = 0;
    int exit_code = -1;

    while (coarse_ticks() - start < TIMEOUT_TICKS) {
        enum sched_poll_result r = scheduler_poll(pid, &exit_code);
        if (r == SCHED_POLL_EXITED) { exited = 1; break; }
        KTEST_ASSERT(r == SCHED_POLL_RUNNING); // never INVALID for a live pid

        // Count ticks rather than loop iterations: an iteration count
        // only proves this loop ran, while a CHANGE in the tick counter
        // proves time passed with the process still alive -- i.e. that
        // the kernel was scheduled back after the process had the CPU,
        // which is the actual claim.
        uint64_t now = coarse_ticks();
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

        uint64_t start = coarse_ticks();
        int exit_code = -1;
        int exited = 0;
        while (coarse_ticks() - start < TIMEOUT_TICKS) {
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
    uint64_t start = coarse_ticks();
    while (coarse_ticks() - start < 20) { }
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
    start = coarse_ticks();
    while (coarse_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    if (!exited) {
        // Same reasoning as win_input_test.c's report_stuck(): "it did
        // not exit" names nothing, and this one only fails under a trap
        // gate, where the difference between starved, parked and left
        // unschedulable is the whole question.
        struct proc_info pi;
        for (int i = 0; i < scheduler_slot_end(); i++) {
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

KTEST("sched", "a slot past the first chunk gets its own guarded kernel stack") {
    // Kernel stacks are allocated 64 slots at a time, at run time, and the
    // guard page below each is unmapped then -- past the boot pool of page
    // tables. A chunk whose guards were skipped would boot and run every
    // test above, and turn the first deep call chain into silent
    // corruption of the stack below.
    static uint64_t tf[SCHED_SLOT_CHUNK_TEST + 1][SCHED_TF_SLOTS];
    static const char chan;
    int got[SCHED_SLOT_CHUNK_TEST + 1];
    int n = 0, far = -1;
    scheduler_preempt_disable();
    while (n <= SCHED_SLOT_CHUNK_TEST) {
        int s = scheduler_test_park(tf[n], &chan, SCHED_WAIT_EVENT);
        if (s < 0) break;
        got[n++] = s;
        if (s >= SCHED_SLOT_CHUNK_TEST) { far = s; break; }
    }
    uint64_t base = far >= 0 ? scheduler_kstack_base(far) : 0;
    uint64_t guard_leaf = base ? paging_kernel_leaf(base - 4096) : 1;
    uint64_t stack_leaf = base ? paging_kernel_leaf(base) : 0;
    for (int i = 0; i < n; i++) scheduler_test_release(got[i]);
    (void)scheduler_test_take_resched();
    scheduler_preempt_enable();

    if (scheduler_max_procs() <= SCHED_SLOT_CHUNK_TEST)
        KTEST_SKIP("the process limit is one chunk on this machine");
    KTEST_ASSERT(far >= SCHED_SLOT_CHUNK_TEST);
    KTEST_ASSERT_EQ(guard_leaf & 1, 0);   // the guard: not present
    KTEST_ASSERT(stack_leaf & 1);         // the stack itself: there
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
    // The kernel context runs this test in the fair class. An RT wake
    // must ask for a switch on the way out of the trap; a fair one
    // parked at a syscall entry must not, or every interrupt would
    // rotate the CPU.
    uint64_t tf_hi[SCHED_TF_SLOTS] = {0}, tf_eq[SCHED_TF_SLOTS] = {0};
    static const char chan_hi, chan_eq;

    scheduler_preempt_disable();
    (void)scheduler_test_take_resched();

    int hi = scheduler_test_park(tf_hi, &chan_hi, SCHED_WAIT_EVENT);
    int eq = scheduler_test_park(tf_eq, &chan_eq, SCHED_WAIT_EVENT);
    int asked_eq = -1, asked_hi = -1, cur = scheduler_current_pid();

    if (hi >= 0 && eq >= 0) {
        scheduler_set_class(scheduler_slot_pid(hi), SCHED_FIFO, 10);
        (void)scheduler_test_take_resched();   // the class change asks for one itself
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

KTEST("sched", "within the fair class, whoever has run least runs next") {
    // Two READY fair slots, far below the pack's floor so nothing real
    // competes. Equal: the scan order decides, so from A's position it
    // is B (the old round-robin). A behind: A, although B comes first in
    // the scan -- which is what a preempted process, or one back from a
    // long wait, needs.
    uint64_t tf_a[SCHED_TF_SLOTS] = {0}, tf_b[SCHED_TF_SLOTS] = {0};
    static const char chan_a, chan_b;

    scheduler_preempt_disable();
    int a = scheduler_test_park(tf_a, &chan_a, SCHED_WAIT_EVENT);
    int b = scheduler_test_park(tf_b, &chan_b, SCHED_WAIT_EVENT);
    int equal = -2, behind = -2;

    if (a >= 0 && b >= 0) {
        scheduler_wake(&chan_a, 0);
        scheduler_wake(&chan_b, 0);
        (void)scheduler_test_take_resched();
        // Small figures: below the floor, so the picks move nothing.
        scheduler_test_set_vruntime(a, 0);
        scheduler_test_set_vruntime(b, 0);
        equal = scheduler_test_pick(a);
        scheduler_test_set_vruntime(b, 1000000);
        behind = scheduler_test_pick(a);
    }

    scheduler_test_release(a);
    scheduler_test_release(b);
    scheduler_preempt_enable();

    if (a < 0 || b < 0) KTEST_SKIP("no free process slots to fabricate");

    KTEST_ASSERT_EQ(equal, b);
    KTEST_ASSERT_EQ(behind, a);
}

KTEST("sched", "RT runs before every fair process: best priority, then FIFO order") {
    // A fair slot that has run NOTHING still loses to any RT one -- the
    // class is a rank, the vruntime only orders the fair class. Within
    // RT, the higher priority first; at one priority, whoever got in
    // line first. Dropping the best to SCHED_OTHER hands over.
    static uint64_t tf[4][SCHED_TF_SLOTS];
    static const char chan[4];
    int s[4], picks[4] = { -2, -2, -2, -2 };

    scheduler_preempt_disable();
    for (int i = 0; i < 4; i++) s[i] = scheduler_test_park(tf[i], &chan[i], SCHED_WAIT_EVENT);
    int ok = s[0] >= 0 && s[1] >= 0 && s[2] >= 0 && s[3] >= 0;
    if (ok) {
        scheduler_set_class(scheduler_slot_pid(s[0]), SCHED_FIFO, 10);
        scheduler_set_class(scheduler_slot_pid(s[1]), SCHED_FIFO, 20);
        scheduler_set_class(scheduler_slot_pid(s[3]), SCHED_FIFO, 10);
        for (int i = 0; i < 4; i++) scheduler_wake(&chan[i], 0);   // 0 in line before 3
        scheduler_test_set_vruntime(s[2], 0);
        picks[0] = scheduler_test_pick(s[2]);        // 20 beats 10 beats fair
        scheduler_set_class(scheduler_slot_pid(s[1]), SCHED_OTHER, 0);
        picks[1] = scheduler_test_pick(s[0]);        // 0 and 3 at 10: 0 woke first
        scheduler_set_class(scheduler_slot_pid(s[0]), SCHED_OTHER, 0);
        scheduler_set_class(scheduler_slot_pid(s[3]), SCHED_OTHER, 0);
        scheduler_test_set_vruntime(s[0], 1000000);
        scheduler_test_set_vruntime(s[1], 1000000);
        scheduler_test_set_vruntime(s[3], 1000000);
        picks[2] = scheduler_test_pick(s[0]);        // all fair: least run
        (void)scheduler_test_take_resched();
    }
    for (int i = 0; i < 4; i++) scheduler_test_release(s[i]);
    scheduler_preempt_enable();

    if (!ok) KTEST_SKIP("no free process slots to fabricate");
    KTEST_ASSERT_EQ(picks[0], s[1]);
    KTEST_ASSERT_EQ(picks[1], s[0]);
    KTEST_ASSERT_EQ(picks[2], s[2]);
}

KTEST("sched", "nice is a weight: CFS's ~1.25x a step, not a rank") {
    uint64_t tf[SCHED_TF_SLOTS] = {0};
    static const char chan;
    uint64_t at0 = 0, at5 = 0, atm5 = 0, at19 = 0;

    scheduler_preempt_disable();
    int a = scheduler_test_park(tf, &chan, SCHED_WAIT_EVENT);
    if (a >= 0) {
        int pid = scheduler_slot_pid(a);
        at0 = scheduler_test_vr_scale(a, 1000000);
        scheduler_set_priority(pid, 5);
        at5 = scheduler_test_vr_scale(a, 1000000);
        scheduler_set_priority(pid, -5);
        atm5 = scheduler_test_vr_scale(a, 1000000);
        scheduler_set_priority(pid, 19);
        at19 = scheduler_test_vr_scale(a, 1000000);
    }
    scheduler_test_release(a);
    scheduler_preempt_enable();

    if (a < 0) KTEST_SKIP("no free process slots to fabricate");
    KTEST_ASSERT_EQ(at0, 1000000);               // nice 0 is real time
    KTEST_ASSERT_EQ(at5, 3056716);               // 1 ms * 1024 / 335
    KTEST_ASSERT_EQ(atm5, 328099);              // 1 ms * 1024 / 3121
    KTEST_ASSERT_EQ(at19, 68266666);             // 1 ms * 1024 / 15
}

KTEST("sched", "an RT process that does not block is moved to SCHED_OTHER") {
    // The watchdog: a second of RT without a block demotes. The budget
    // is put back after, or real RT processes would start throttled.
    static uint64_t tf[2][SCHED_TF_SLOTS];
    static const char chan[2];
    int policy_short = -1, policy_long = -1, rt_prio = -1;
    uint32_t wd = scheduler_rt_watchdog_ms();

    scheduler_preempt_disable();
    int a = scheduler_test_park(tf[0], &chan[0], SCHED_WAIT_EVENT);
    int b = scheduler_test_park(tf[1], &chan[1], SCHED_WAIT_EVENT);
    if (a >= 0 && b >= 0 && wd) {
        scheduler_set_class(scheduler_slot_pid(a), SCHED_FIFO, 5);
        scheduler_set_class(scheduler_slot_pid(b), SCHED_RR, 5);
        scheduler_test_rt_charge(a, (uint64_t)(wd - 1) * 1000000ull);
        scheduler_get_class(scheduler_slot_pid(a), &policy_short, &rt_prio);
        scheduler_test_rt_charge(b, (uint64_t)wd * 1000000ull);
        scheduler_get_class(scheduler_slot_pid(b), &policy_long, &rt_prio);
        scheduler_test_rt_budget_reset();
        (void)scheduler_test_take_resched();
    }
    scheduler_test_release(a);
    scheduler_test_release(b);
    scheduler_preempt_enable();

    if (a < 0 || b < 0) KTEST_SKIP("no free process slots to fabricate");
    if (!wd) KTEST_SKIP("kernel.sched_rt_watchdog_ms is 0");
    KTEST_ASSERT_EQ(policy_short, SCHED_FIFO);   // just under: kept
    KTEST_ASSERT_EQ(policy_long, SCHED_OTHER);   // at the limit: demoted
    KTEST_ASSERT_EQ(rt_prio, 0);
}

KTEST("sched", "throttled RT yields to a READY fair process") {
    // Past kernel.sched_rt_runtime_ms in this second, RT is passed over
    // while an ordinary process is READY. (That it still runs when none
    // is cannot be fabricated here: real processes may be READY too.)
    static uint64_t tf[2][SCHED_TF_SLOTS];
    static const char chan[2];
    int fresh = -2, throttled = -2;
    uint32_t rt = scheduler_rt_runtime_ms();

    scheduler_preempt_disable();
    int r = scheduler_test_park(tf[0], &chan[0], SCHED_WAIT_EVENT);
    int f = scheduler_test_park(tf[1], &chan[1], SCHED_WAIT_EVENT);
    if (r >= 0 && f >= 0 && rt < 1000) {
        scheduler_set_class(scheduler_slot_pid(r), SCHED_FIFO, 5);
        scheduler_wake(&chan[0], 0);
        scheduler_wake(&chan[1], 0);
        scheduler_test_set_vruntime(f, 0);
        scheduler_test_rt_budget_reset();
        fresh = scheduler_test_pick(f);                    // the control
        scheduler_test_rt_charge(r, (uint64_t)rt * 1000000ull);
        throttled = scheduler_test_pick(f);
        scheduler_test_rt_budget_reset();
        (void)scheduler_test_take_resched();
    }
    scheduler_test_release(r);
    scheduler_test_release(f);
    scheduler_preempt_enable();

    if (r < 0 || f < 0) KTEST_SKIP("no free process slots to fabricate");
    if (rt >= 1000) KTEST_SKIP("kernel.sched_rt_runtime_ms is unlimited");
    KTEST_ASSERT_EQ(fresh, r);
    KTEST_ASSERT_EQ(throttled, f);
}

KTEST("sched", "a mid-call wake cuts in only while it has run less than what runs") {
    // THE LIVELOCK THIS REPLACED: a disk-bound thread woken by every IRQ
    // cut in unconditionally, and with the kernel context it held the CPU
    // while every other READY process waited forever. Ahead: no switch.
    // Behind: a switch, which is why the cut-in exists at all (it
    // usually holds the filesystem lock) -- the control.
    uint64_t tf[SCHED_TF_SLOTS] = {0};
    static const char chan;

    scheduler_preempt_disable();
    (void)scheduler_test_take_resched();
    uint64_t kv = scheduler_test_vruntime(-1), base = scheduler_test_vruntime(-2);
    int w = scheduler_test_park(tf, &chan, SCHED_WAIT_DISK);
    int cur = scheduler_current_pid(), ahead = -1, behind = -1;

    if (w >= 0 && !cur) {
        scheduler_test_set_vruntime(-1, base + 10000000000ull);   // the kernel: 10 s in
        scheduler_test_park_deadline(w, 0, 1);                    // parked mid-call
        scheduler_test_set_vruntime(w, base + 20000000000ull);    // 20 s: ahead of it
        scheduler_wake(&chan, 0);
        ahead = scheduler_test_take_resched();

        scheduler_test_release(w);
        w = scheduler_test_park(tf, &chan, SCHED_WAIT_DISK);
        if (w >= 0) {
            scheduler_test_park_deadline(w, 0, 1);
            scheduler_test_set_vruntime(w, base);                 // 10 s behind
            scheduler_wake(&chan, 0);
            behind = scheduler_test_take_resched();
        }
    }

    scheduler_test_set_vruntime(-1, kv);
    if (w >= 0) {
        scheduler_test_park_deadline(w, 0, 0);
        scheduler_test_release(w);
    }
    scheduler_preempt_enable();

    if (cur) KTEST_SKIP("not run from the kernel context");
    if (w < 0) KTEST_SKIP("no free process slots to fabricate");

    KTEST_ASSERT_EQ(ahead, 0);
    KTEST_ASSERT_EQ(behind, 1);
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

KTEST("sched", "a deadline releases a mid-call park without answering it") {
    // A context parked MID-CALL resumes its own C frames; its kernel_rsp
    // is the trapframe of a syscall still in progress, so a timer that
    // writes SYS_RETRY there corrupts that call's eventual return value.
    // The entry-parked slot beside it is the control: same deadline,
    // and it MUST be answered.
    uint64_t tf_k[SCHED_TF_SLOTS] = {0}, tf_e[SCHED_TF_SLOTS] = {0};
    static const char chan_k, chan_e;
    const uint64_t SENTINEL = 0x5157;
    tf_k[SCHED_TF_RAX] = SENTINEL;
    tf_e[SCHED_TF_RAX] = SENTINEL;

    scheduler_preempt_disable();
    int k = scheduler_test_park(tf_k, &chan_k, SCHED_WAIT_DISK);
    int e = scheduler_test_park(tf_e, &chan_e, SCHED_WAIT_NET);
    int state_k = -1, state_e = -1;
    uint64_t rax_k = 0, rax_e = 0;
    if (k >= 0 && e >= 0) {
        scheduler_test_park_deadline(k, 1, 1);   // long past
        scheduler_test_park_deadline(e, 1, 0);
        scheduler_wake_timers(2);
        state_k = scheduler_test_state(k);
        state_e = scheduler_test_state(e);
        rax_k = tf_k[SCHED_TF_RAX];
        rax_e = tf_e[SCHED_TF_RAX];
    }
    scheduler_test_park_deadline(k, 0, 0);
    scheduler_test_release(k);
    scheduler_test_release(e);
    scheduler_test_take_resched();
    scheduler_preempt_enable();

    if (k < 0 || e < 0) KTEST_SKIP("no free process slots to fabricate");
    KTEST_ASSERT_EQ(state_k, PROC_STATE_READY);    // released...
    KTEST_ASSERT_EQ(rax_k, SENTINEL);              // ...and NOT answered
    KTEST_ASSERT_EQ(state_e, PROC_STATE_READY);
    KTEST_ASSERT_EQ((int64_t)rax_e, (int64_t)SYS_RETRY); // the control was
}

KTEST("sched", "a kill waits for a context WOKEN mid-call, not only a parked one") {
    // Woken but not yet resumed, it is still inside its kernel frames and
    // may hold the filesystem lock; tearing it down orphans the lock and
    // every later file call waits forever. Measured: the desktop killing
    // its screensaver did exactly that. The kill must become a pending
    // SIGKILL and leave the slot READY.
    uint64_t tf[SCHED_TF_SLOTS] = {0};
    static const char chan;

    scheduler_preempt_disable();
    int w = scheduler_test_park(tf, &chan, SCHED_WAIT_DISK);
    int state = -1;
    uint32_t pending = 0;
    if (w >= 0) {
        scheduler_test_park_deadline(w, 0, 1);   // parked mid-call
        scheduler_wake(&chan, 0);                // ...and woken: READY
        (void)scheduler_test_take_resched();
        scheduler_kill(scheduler_slot_pid(w), 1);
        state = scheduler_test_state(w);
        pending = scheduler_signal_pending(scheduler_slot_pid(w));
        scheduler_test_release(w);
    }
    scheduler_preempt_enable();

    if (w < 0) KTEST_SKIP("no free process slots to fabricate");
    KTEST_ASSERT_EQ(state, PROC_STATE_READY);
    KTEST_ASSERT(pending & (1u << SIGKILL));
}

KTEST("sched", "a kill waits for ANY thread parked mid-call, not only the leader") {
    // A thread parked mid-call holds what its C frames hold -- the disk
    // lock, a mount's -- and freeing it with its process orphans that
    // lock for the rest of the boot. Measured: System Update killed while
    // its worker thread hashed files froze the desktop. The leader here
    // is parked at a syscall entry, so only the THREAD makes the kill wait.
    uint64_t tf_l[SCHED_TF_SLOTS] = {0}, tf_t[SCHED_TF_SLOTS] = {0};
    static const char chan_l, chan_t;

    scheduler_preempt_disable();
    int l = scheduler_test_park(tf_l, &chan_l, SCHED_WAIT_NET);
    int t = scheduler_test_park(tf_t, &chan_t, SCHED_WAIT_DISK);
    int state_l = -1, state_t = -1, polled = -1, rc = -1;
    uint32_t pend_l = 0, pend_t = 0;
    if (l >= 0 && t >= 0) {
        scheduler_test_make_thread(t, l);
        scheduler_test_park_deadline(t, 0, 1);   // the thread: mid-call
        rc = scheduler_kill(scheduler_slot_pid(l), 9);
        state_l = scheduler_test_state(l);
        state_t = scheduler_test_state(t);
        pend_l = scheduler_signal_pending(scheduler_slot_pid(l));
        pend_t = scheduler_signal_pending(scheduler_slot_pid(t));
        polled = (int)scheduler_poll(scheduler_slot_pid(l), 0);
    }
    scheduler_test_release(t);
    scheduler_test_release(l);
    (void)scheduler_test_take_resched();
    scheduler_preempt_enable();

    if (l < 0 || t < 0) KTEST_SKIP("fewer than two free process slots");
    KTEST_ASSERT_EQ(rc, 1);                          // accepted...
    KTEST_ASSERT_EQ(state_t, PROC_STATE_BLOCKED);    // ...the thread lives on
    KTEST_ASSERT(pend_t & (1u << SIGKILL));          // and dies on its way out
    KTEST_ASSERT(pend_l & (1u << SIGKILL));          // the leader too
    KTEST_ASSERT(state_l != PROC_STATE_ZOMBIE);
    KTEST_ASSERT_EQ(polled, (int)SCHED_POLL_RUNNING); // nothing to reap yet
}

KTEST("sched", "a reaped pid is not handed straight to the next process") {
    // Pids were slot numbers, so the pid a program had just held went to
    // the very next spawn: every launch of one app was pid 4, and anything
    // still holding the old pid (a parent, a terminal's owner) now named a
    // stranger.
    uint64_t tf[SCHED_TF_SLOTS] = {0};
    static const char chan;
    scheduler_preempt_disable();
    int a = scheduler_test_park(tf, &chan, SCHED_WAIT_EVENT);
    int pa = scheduler_slot_pid(a);
    scheduler_test_release(a);
    int b = scheduler_test_park(tf, &chan, SCHED_WAIT_EVENT);
    int pb = scheduler_slot_pid(b);
    scheduler_test_release(b);
    scheduler_preempt_enable();

    if (a < 0 || b < 0) KTEST_SKIP("no free process slot");
    KTEST_ASSERT(pa > 0 && pb > 0);
    KTEST_ASSERT(pb != pa);
}

KTEST("sched", "allocation skips a live pid and wraps to SCHED_PID_RESERVED") {
    uint64_t tf_a[SCHED_TF_SLOTS] = {0}, tf_b[SCHED_TF_SLOTS] = {0}, tf_c[SCHED_TF_SLOTS] = {0};
    static const char chan;
    scheduler_preempt_disable();
    int a = scheduler_test_park(tf_a, &chan, SCHED_WAIT_EVENT);
    int pa = scheduler_slot_pid(a);
    scheduler_test_set_last_pid(pa - 1);          // the next candidate IS pa
    int b = scheduler_test_park(tf_b, &chan, SCHED_WAIT_EVENT);
    int pb = scheduler_slot_pid(b);
    scheduler_test_set_last_pid(SCHED_PID_MAX - 1);
    int c = scheduler_test_park(tf_c, &chan, SCHED_WAIT_EVENT);
    int pc = scheduler_slot_pid(c);
    scheduler_test_release(c);
    scheduler_test_release(b);
    scheduler_test_release(a);
    scheduler_preempt_enable();

    if (a < 0 || b < 0 || c < 0) KTEST_SKIP("fewer than three free process slots");
    KTEST_ASSERT(pb != pa);                        // skipped the live one
    KTEST_ASSERT(pc >= SCHED_PID_RESERVED && pc < SCHED_PID_MAX - 1);   // wrapped low
    KTEST_ASSERT(pc != 1);                         // ...but never to init's
}

KTEST("sched", "a pid still named as a process group is not handed out") {
    // Linux keeps a struct pid alive while a group or session names it;
    // reusing the number would put a new process in an old group.
    uint64_t tf_a[SCHED_TF_SLOTS] = {0}, tf_b[SCHED_TF_SLOTS] = {0};
    static const char chan;
    scheduler_preempt_disable();
    int a = scheduler_test_park(tf_a, &chan, SCHED_WAIT_EVENT);
    const int X = SCHED_PID_MAX - 2;               // a number no live slot holds
    scheduler_test_set_pgid(a, X);
    scheduler_test_set_last_pid(X - 1);
    int b = scheduler_test_park(tf_b, &chan, SCHED_WAIT_EVENT);
    int pb = scheduler_slot_pid(b);
    scheduler_test_release(b);
    scheduler_test_release(a);
    scheduler_preempt_enable();

    if (a < 0 || b < 0) KTEST_SKIP("fewer than two free process slots");
    KTEST_ASSERT(pb != X);
}

KTEST("sched", "init is pid 1") {
    int init = scheduler_init_pid();
    if (!init) KTEST_SKIP("this boot has no init");
    KTEST_ASSERT_EQ(init, 1);
}

KTEST("sched", "a slot being built is never handed out twice") {
    // A spawn claims its slot and then SLEEPS reading the ELF. The slot
    // stayed UNUSED meanwhile, so a second spawn took the same one and
    // the first child never existed -- its parent then read its pipe
    // forever. A claimed slot must be skipped by the next claim, and
    // must still look like nothing to every other scan of the table.
    scheduler_preempt_disable();
    int a = scheduler_test_slot_claim();
    int b = scheduler_test_slot_claim();
    int state_a = a >= 0 ? scheduler_test_state(a) : -1;
    scheduler_test_slot_unclaim(a);
    scheduler_test_slot_unclaim(b);
    scheduler_preempt_enable();

    if (a < 0 || b < 0) KTEST_SKIP("fewer than two free process slots");
    KTEST_ASSERT(a != b);
    KTEST_ASSERT_EQ(state_a, PROC_STATE_UNUSED);   // not a process to anyone else
}

