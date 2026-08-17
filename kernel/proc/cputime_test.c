// CPU accounting: nobody is billed more time than actually elapsed.
//
// The interesting assertion cannot be written in kernel space alone --
// it needs a real scheduler-managed process spinning on SYS_YIELD, and
// the KTEST itself is not one. So it lives in userland/tests/
// cputime_test.c and this drives it, using its exit code as the verdict,
// exactly as pipe_test.c does for the same reason.
//
// It also cannot be driven by `run`/usertest_run.py: that is the legacy
// process_run_ring3() path, which has NO procs[] entry, so the test
// cannot find its own slot and nothing is billed to it in the first
// place. Spawning it properly is the whole point.
//
// THE BUG: SYS_YIELD rescheduled by calling scheduler_tick(), which
// charges the running process a whole tick. A yield elapses
// microseconds, so a polling GUI app billed itself thousands of ticks a
// second against the PIT's hundred and reported a clamped 100% -- as did
// every other polling app at the same time, which on one CPU is
// impossible. See scheduler_rotate().
#include "ktest.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"

#define CPUTIME_TEST_PATH "/tests/cputime_test"

// The child spins for ~30 ticks and this polls it; generous enough that
// a busy desktop cannot time it out, still under a second in practice.
#define TIMEOUT_TICKS 500

KTEST("sched", "a yielding process is not billed more ticks than elapsed") {
    if (!fs_exists(CPUTIME_TEST_PATH)) KTEST_SKIP("no " CPUTIME_TEST_PATH " on this boot");

    uint64_t t0 = pit_ticks();
    int pid = scheduler_spawn(CPUTIME_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    while (pit_ticks() - t0 < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // Non-zero means the child measured more CPU billed to itself than
    // wall-clock time passed -- see userland/tests/cputime_test.c, which
    // prints both figures to stderr before failing, so `dmesg` says by
    // how much rather than only that it happened.
    KTEST_ASSERT_EQ(code, 0);
}

// NOTE the other half of the property -- that a yield still
// RESCHEDULES -- is asserted inside the child, not here: turning
// scheduler_yield() into a no-op would otherwise pass this file
// perfectly. The child requires its own loop to have made real progress
// against the wall clock before it reports success.
