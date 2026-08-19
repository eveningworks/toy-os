// The per-process current directory, driven through a real ring-3
// process.
//
// The properties are userland/tests/cwd_test.c's; this exists to SPAWN
// it properly. Its load-bearing check spawns /bin/mkdir with a bare name
// and waits for it, and under the shell's `run` the legacy blocking
// loader has no scheduler slot -- so its waitpid cannot park, returns
// immediately, and the test looks for the child's directory before the
// child has created it. That reports an inheritance failure which is
// really a harness limitation. Same reason fd_test and pipe_test are
// covered here rather than by tools/usertest_run.py; see that file's
// EXCLUDED list.
#include "ktest.h"
#include "scheduler.h"
#include "fs.h"
#include "timer.h"

#define CWD_TEST_PATH "/tests/cwd_test"
#define TIMEOUT_TICKS 600 // 6s at the PIT's 100Hz

KTEST("cwd", "chdir/getcwd, the path syscalls, and inheritance across spawn") {
    if (!fs_exists(CWD_TEST_PATH)) KTEST_SKIP("no " CWD_TEST_PATH " on this boot");
    // The inheritance check spawns this one, so its absence would fail
    // the test for a reason that is not about the kernel.
    if (!fs_exists("/bin/mkdir")) KTEST_SKIP("no /bin/mkdir on this boot");

    int pid = scheduler_spawn(CWD_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // The exit code is the number of FAILED checks, so this says how
    // many rather than only that something broke; the individual lines
    // are on the serial console either way.
    KTEST_ASSERT_EQ(code, 0);
}
