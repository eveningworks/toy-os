// The descriptor table, driven through a real ring-3 process.
//
// The properties are userland/tests/fd_test.c's; this exists to SPAWN
// it properly. Under the shell's `run` the legacy blocking loader has
// no scheduler slot, so the test's own `waitpid` cannot park and
// returns immediately -- it then reads the child's output file before
// the child has run, and reports an inheritance failure that is really
// a harness limitation. Exactly why `pipe_test` is excluded from
// tools/usertest_run.py and covered here instead; see that file's
// EXCLUDED list.
#include "ktest.h"
#include "scheduler.h"
#include "fs.h"
#include "timer.h"

#define FD_TEST_PATH "/tests/fd_test"
#define PIPEFULL_PATH "/tests/pipefull_test"
#define TIMEOUT_TICKS 600 // 6s at the PIT's 100Hz

KTEST("fd", "dup, dup2 and inheritance, in a real process") {
    if (!fs_exists(FD_TEST_PATH)) KTEST_SKIP("no " FD_TEST_PATH " on this boot");
    if (!fs_exists("/bin/hello")) KTEST_SKIP("no /bin/hello on this boot");

    int pid = scheduler_spawn(FD_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // The exit code is the number of FAILED checks, so this reports how
    // many rather than only that something broke. The individual lines
    // are on the serial console either way.
    KTEST_ASSERT_EQ(code, 0);
}

// A pipe that fills must BLOCK its writer rather than truncating.
//
// Spawned rather than `run`, and that is not incidental: the legacy
// loader has no scheduler slot, so it CANNOT park -- a full-pipe write
// there reports 0 instead of blocking, and the test would fail against
// a kernel that is behaving correctly. The trace showed exactly that
// before this KTEST existed.
KTEST("fd", "a full pipe blocks its writer instead of truncating") {
    if (!fs_exists(PIPEFULL_PATH)) KTEST_SKIP("no " PIPEFULL_PATH " on this boot");
    if (!fs_exists("/tests/pipedrain")) KTEST_SKIP("no /tests/pipedrain on this boot");

    int pid = scheduler_spawn(PIPEFULL_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    // Not exiting is itself the failure this guards: before the writer
    // could park, the pair deadlocked and sat blocked forever.
    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(code, 0);
}
