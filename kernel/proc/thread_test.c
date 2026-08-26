// Threads: the kernel-side invariants, plus the ring-3 test that cannot
// be written here.
//
// The split is the same one cputime_test.c makes. What a KTEST can
// assert directly is the shape of the process table -- that a thread
// shares its leader's address space, that it is not a child, that
// killing any of them kills all of them. What it cannot do is CREATE
// one: SYS_THREAD_CREATE runs on the calling process's address space,
// and a KTEST runs in the kernel context, which has none. So the
// creation side lives in userland/tests/thread_test.c and this drives
// it by exit code.
#include "ktest.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"
#include "proc_info.h"
#include "string.h" // k_strcmp -- a thread carries its leader's name

#define THREAD_TEST_PATH "/tests/thread_test"

// It creates and joins a dozen threads and does no I/O beyond one small
// file; generous enough that a busy desktop cannot time it out.
#define TIMEOUT_TICKS 1500

KTEST("thread", "a ring-3 program can create, join and share with threads") {
    if (!fs_exists(THREAD_TEST_PATH)) KTEST_SKIP("no " THREAD_TEST_PATH " on this boot");

    uint64_t t0 = pit_ticks();
    int pid = scheduler_spawn(THREAD_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1, exited = 0;
    while (pit_ticks() - t0 < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // The exit code is the number of failed checks. The child names
    // each one on its own stdout and in /tmp/thread_test.out, so a
    // failure here is diagnosable from `dmesg` rather than only
    // countable.
    KTEST_ASSERT_EQ(code, 0);
}

// A PROCESS THAT LEAVES THREADS BEHIND WOULD LEAK SLOTS FOREVER, and
// nothing in the child could notice: it is dead by the time the
// question is asked. So it is asked from out here, across the whole
// table, before and after.
//
// This is also the only check that would catch the group teardown being
// dropped -- every ring-3 assertion passes with it removed, because a
// leaked thread does no harm to the program that spawned it.
KTEST("thread", "a process's threads are gone when it is") {
    if (!fs_exists(THREAD_TEST_PATH)) KTEST_SKIP("no " THREAD_TEST_PATH " on this boot");

    int before = 0;
    for (int i = 0; i < scheduler_max_procs(); i++) {
        struct proc_info info;
        if (scheduler_proc_info(i, &info) && info.pid) before++;
    }

    uint64_t t0 = pit_ticks();
    int pid = scheduler_spawn(THREAD_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);
    int code = -1, exited = 0;
    while (pit_ticks() - t0 < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);

    // The spawn is reaped by the poll above, so the table must be back
    // where it started -- one leaked thread makes this larger.
    int after = 0;
    for (int i = 0; i < scheduler_max_procs(); i++) {
        struct proc_info info;
        if (scheduler_proc_info(i, &info) && info.pid) after++;
    }
    KTEST_ASSERT_EQ(after, before);
}

// WHAT `ps --threads` READS. The reporting path is separate from the
// scheduling one -- a thread can work perfectly while proc_info reports
// it as an unrelated process -- so this asserts the three fields that
// make a thread recognisable from ring 3: it names its leader's group,
// it is not its own leader, and it carries its leader's name.
KTEST("thread", "a thread is reported as one, with its leader's identity") {
    if (!fs_exists(THREAD_TEST_PATH)) KTEST_SKIP("no " THREAD_TEST_PATH " on this boot");

    uint64_t t0 = pit_ticks();
    int pid = scheduler_spawn(THREAD_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    struct proc_info leader, thread;
    int found = 0, code = 0;
    // Poll until one appears. The child creates its first thread within
    // a few instructions of starting, and the timeout is the same one
    // its own run gets.
    while (pit_ticks() - t0 < TIMEOUT_TICKS && !found) {
        for (int i = 0; i < scheduler_max_procs() && !found; i++) {
            struct proc_info info;
            if (!scheduler_proc_info(i, &info) || !info.pid) continue;
            if (info.tgid == info.pid) continue;   // a process, not a thread
            if (info.tgid != pid) continue;         // somebody else's thread
            thread = info;
            found = 1;
        }
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) break;
    }
    KTEST_ASSERT(found);
    KTEST_ASSERT(scheduler_proc_info(pid - 1, &leader));
    KTEST_ASSERT_EQ(thread.tgid, leader.pid);
    KTEST_ASSERT(thread.pid != leader.pid);
    // A thread has no name of its own -- `ps` prints its leader's in
    // braces, and that only works because the kernel copies it.
    KTEST_ASSERT(k_strcmp(thread.name, leader.name) == 0);
    // Its parent is its leader, which is what nests it under the right
    // process in `ps --tree`.
    KTEST_ASSERT_EQ(thread.ppid, leader.pid);

    while (pit_ticks() - t0 < TIMEOUT_TICKS)
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) break;
}

// THE TGID IS WHAT TELLS A THREAD FROM A PROCESS, so an ordinary
// process must lead its own group -- otherwise every walk that skips
// threads would skip everything.
KTEST("thread", "an ordinary process is its own thread group") {
    int pid = scheduler_spawn("/bin/hello", 0);
    if (!pid) KTEST_SKIP("no /bin/hello on this boot");

    struct proc_info info;
    KTEST_ASSERT(scheduler_proc_info(pid - 1, &info));
    KTEST_ASSERT_EQ(info.tgid, pid);
    KTEST_ASSERT_EQ(scheduler_tgid(pid), pid);

    uint64_t t0 = pit_ticks();
    int code = 0;
    while (pit_ticks() - t0 < TIMEOUT_TICKS)
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) break;
}

// The kernel context is not a process, so it has no group -- and every
// caller that asks gets 0 rather than a slot number that happens to be
// lying around.
KTEST("thread", "the kernel context has no thread group") {
    KTEST_ASSERT_EQ(scheduler_current_tgid(), 0);
    KTEST_ASSERT_EQ(scheduler_tgid(0), 0);
    KTEST_ASSERT_EQ(scheduler_tgid(scheduler_max_procs() + 1), 0);
}

// A thread pointer is per THREAD, and setting one from the kernel
// context has to land somewhere too -- that is the legacy loader's
// slot, which switch_to_kernel() reloads. Refusing here instead (which
// it did for one build) makes every ring-3 program die in crt0, since
// errno is thread-local now.
KTEST("thread", "the kernel context can still set a thread pointer") {
    KTEST_ASSERT_EQ(scheduler_set_tls(0), 0);
}
