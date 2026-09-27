// Tests for pipes and the spawn/wait chain built on them.
//
// The end-to-end case is the one that matters and it cannot be written
// in kernel space alone: "a ring-3 process reads another ring-3
// process's stdout" needs two real processes. So the assertion lives in
// userland/pipe_test.c and this drives it, using its exit code as the
// verdict -- each failure mode there has its own code, so a failure
// says which link broke rather than just that something did.
#include "ktest.h"
#include "pipe.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"

#define PIPE_TEST_PATH "/tests/pipe_test"
#define TIMEOUT_TICKS 500

KTEST("pipe", "a written byte comes back out, in order") {
    int p = pipe_create();
    KTEST_ASSERT(p >= 0);

    KTEST_ASSERT_EQ(pipe_write(p, "abc", 3), 3);
    char buf[8];
    KTEST_ASSERT_EQ(pipe_read(p, buf, sizeof buf), 3);
    KTEST_ASSERT_EQ(buf[0], 'a');
    KTEST_ASSERT_EQ(buf[2], 'c');

    pipe_close_writer(p);
    pipe_close_reader(p);
}

KTEST("pipe", "empty with a live writer is WOULD-BLOCK, without one is EOF") {
    int p = pipe_create();
    KTEST_ASSERT(p >= 0);
    char buf[4];

    // The distinction this whole widget exists to make: -1 means "wait",
    // 0 means "there will never be more". Conflating them is how a
    // terminal decides a still-running program has already finished.
    KTEST_ASSERT_EQ(pipe_read(p, buf, sizeof buf), -1);
    pipe_close_writer(p);
    KTEST_ASSERT_EQ(pipe_read(p, buf, sizeof buf), 0);
    pipe_close_reader(p);
}

KTEST("pipe", "buffered data survives the writer closing") {
    int p = pipe_create();
    KTEST_ASSERT(p >= 0);

    KTEST_ASSERT_EQ(pipe_write(p, "hi", 2), 2);
    pipe_close_writer(p); // writer gone, but those bytes are still owed
    char buf[4];
    KTEST_ASSERT_EQ(pipe_read(p, buf, sizeof buf), 2);
    KTEST_ASSERT_EQ(buf[0], 'h');
    KTEST_ASSERT_EQ(pipe_read(p, buf, sizeof buf), 0); // NOW it is EOF
    pipe_close_reader(p);
}

KTEST("pipe", "a write with no readers is discarded, not buffered forever") {
    int p = pipe_create();
    KTEST_ASSERT(p >= 0);
    pipe_close_reader(p);
    KTEST_ASSERT_EQ(pipe_write(p, "x", 1), 0);
    pipe_close_writer(p);
}

// The whole chain, through two real ring-3 processes.
KTEST("pipe", "a process reads another process's stdout and reaps it") {
    if (!fs_exists(PIPE_TEST_PATH)) KTEST_SKIP("no " PIPE_TEST_PATH " on this boot");
    if (!fs_exists("/bin/hello")) KTEST_SKIP("no /bin/hello on this boot");

    int pid = scheduler_spawn(PIPE_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    uint64_t start = coarse_ticks();
    while (coarse_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // 0 = every link worked. See userland/pipe_test.c for what each
    // other code means; they are distinct so this reports WHICH link
    // broke rather than only that something did.
    KTEST_ASSERT_EQ(code, 0);
}
