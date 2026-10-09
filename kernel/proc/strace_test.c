// The whole trace path, end to end: /tests/trace_test runs /bin/strace
// with its stderr on a pty and reads the master, driven here by its exit
// code -- the arrangement kernel/tty/tty_test.c gives /tests/pty_test.
// Decoding is ring 3's and tested there (/tests/utrace_test); the ring
// is /tests/tracering_test's.
#include "ktest.h"
#include "strace.h"
#include "syscall_abi.h"
#include "kapi.h"
#include "scheduler.h"
#include "fs.h"
#include "timer.h" // coarse_ticks() -- the spawn's timeout

// --- where the trace comes out ----------------------------------------

#define TRACE_TEST_PATH "/tests/trace_test"
#define TRACE_TIMEOUT_TICKS 600 // 6s at 100Hz

KTEST("strace", "/bin/strace's trace reaches its stderr, and a trace with no ring is refused") {
    if (!fs_exists(TRACE_TEST_PATH)) KTEST_SKIP("no " TRACE_TEST_PATH " on this boot");
    if (!fs_exists("/bin/hello")) KTEST_SKIP("no /bin/hello on this boot");

    int pid = scheduler_spawn(TRACE_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    uint64_t start = coarse_ticks();
    while (coarse_ticks() - start < TRACE_TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // 0 = every phase worked. The codes are distinct so this reports
    // WHICH one broke: 5/6 are the untraced control finding trace text,
    // 8/9 strace's run finding none on its stderr, 11 strace's exit
    // status, 12 a trace with no ring accepted, 10 an unknown spawn flag
    // accepted. See userland/tests/trace_test.c.
    KTEST_ASSERT_EQ(code, 0);
}
