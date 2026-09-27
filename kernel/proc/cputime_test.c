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
#include "clocksource.h"
#include "fs.h"

#define CPUTIME_TEST_PATH "/tests/cputime_test"

// The child spins for ~30 ticks and this polls it; generous enough that
// a busy desktop cannot time it out, still under a second in practice.
#define TIMEOUT_TICKS 500

KTEST("sched", "a yielding process is not billed more ticks than elapsed") {
    if (!fs_exists(CPUTIME_TEST_PATH)) KTEST_SKIP("no " CPUTIME_TEST_PATH " on this boot");

    uint64_t t0 = coarse_ticks();
    int pid = scheduler_spawn(CPUTIME_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    while (coarse_ticks() - t0 < TIMEOUT_TICKS) {
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

// --- the machine-wide split (QUERY_CPULOAD) --------------------------
//
// The counters a caller divides for a system CPU percentage. A KTEST
// runs in the RING-0 context, which is exactly what makes the second
// check discriminating: work done here must land in `kernel_ns`, and a
// version that billed everything to `proc_ns` would look perfectly
// plausible on a busy machine and be wrong on this one.
//
// THE SPIN IS UNDER A PREEMPTION GUARD, and that is the precondition,
// not a shortcut: scheduler_rotate() hands the CPU to any runnable
// ring-3 process on every tick, so beside a busy desktop the kernel
// context gets alternate ticks and the accounting CORRECTLY reports
// half the wall time here (measured: dk=30ms dp=30ms wall=60ms). With
// a tick-granular clocksource one lost tick already fails the strict
// threshold. The guard is what makes "this context did the work" true.
KTEST("sched", "the machine-wide CPU split advances and lands in the right bucket") {
    uint64_t p0 = 0, k0 = 0, p1 = 0, k1 = 0;
    scheduler_preempt_disable();
    scheduler_cpu_time(&p0, &k0);

    // Long enough to be several timer ticks at 100 Hz, so the result
    // does not depend on the clocksource's resolution.
    uint64_t start = clocksource_now_ns();
    volatile uint64_t sink = 0;
    while (clocksource_now_ns() - start < 50ull * 1000 * 1000) sink++;
    (void)sink;

    scheduler_cpu_time(&p1, &k1);
    uint64_t wall = clocksource_now_ns() - start;
    scheduler_preempt_enable();

    // Neither counter may go backwards -- they are cumulative since boot.
    KTEST_ASSERT(p1 >= p0);
    KTEST_ASSERT(k1 >= k0);

    // The spin was ring-0 work, so it is the KERNEL bucket that moved,
    // and it moved by most of the wall time. A tolerance rather than an
    // equality because the timer interrupt runs during the spin.
    uint64_t dk = k1 - k0, dp = p1 - p0;
    KTEST_ASSERT(dk > 40ull * 1000 * 1000);
    KTEST_ASSERT(dk > dp);

    // And the two together cannot exceed the wall clock: a slice
    // charged to both buckets, or charged twice, shows up here.
    KTEST_ASSERT(dk + dp <= wall + 10ull * 1000 * 1000);
}

// The reading is FRESH: scheduler_cpu_time() bills the slice in
// progress rather than reporting whatever the last rotation left. Two
// reads either side of a spin must differ even though no rotation is
// forced between them.
KTEST("sched", "the CPU split includes the slice still running") {
    uint64_t k0 = 0, k1 = 0;
    scheduler_preempt_disable();   // same precondition as above
    scheduler_cpu_time(0, &k0);
    uint64_t start = clocksource_now_ns();
    volatile uint64_t sink = 0;
    while (clocksource_now_ns() - start < 20ull * 1000 * 1000) sink++;
    (void)sink;
    scheduler_cpu_time(0, &k1);
    scheduler_preempt_enable();
    KTEST_ASSERT(k1 - k0 > 15ull * 1000 * 1000);
}
