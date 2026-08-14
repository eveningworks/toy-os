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
#include "scheduler.h"
#include "timer.h"
#include "elf_run.h" // elf_run_from_fs() -- the legacy blocking path
#include "fs.h"

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
    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(code, 0);
}
