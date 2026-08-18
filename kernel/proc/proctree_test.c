// The process TREE: parentage, reparenting, and reaping any child.
//
// There was no parent link at all before this -- `struct sched_process`
// knew a process's page tables, its stack and its name, and nothing
// about who started it. So there was no tree to walk, no way to adopt
// an orphan, and no way to ask "has any child of mine died", which is
// the question an init asks all day (docs/init-design.md, stage 0).
//
// These run against the scheduler's own API rather than through
// syscalls, because the thing under test is the bookkeeping and not the
// ABI: a KTEST can spawn, kill and reap with no VM, no injected keys
// and no desktop.
#include "ktest.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"
#include "kapi.h"

#define SPIN_PATH "/tests/spin_test"
#define TIMEOUT_TICKS 200

// The kernel context is not a scheduled process, so anything a KTEST
// spawns has ppid 0 -- "the kernel did it". A KTEST cannot pretend to
// be a process, so where a real parent is needed these use
// scheduler_reparent(), which is the same call stage 1's adoption will
// use rather than a hook that exists only for tests.
static int find_slot(int pid) { return pid - 1; }

KTEST("proctree", "a kernel-spawned process has no parent") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    int pid = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(pid > 0);

    struct proc_info info;
    KTEST_ASSERT(scheduler_proc_info(find_slot(pid), &info));
    KTEST_ASSERT_EQ(info.pid, pid);
    // 0, not the caller's slot: scheduler_current_pid() is 0 in kernel
    // context, and "the kernel started it" is exactly what that means.
    KTEST_ASSERT_EQ(info.ppid, 0);

    scheduler_kill(pid, 0);
    int code = 0;
    scheduler_poll(pid, &code);
}

KTEST("proctree", "waiting for ANY child tells apart 'not yet' from 'never'") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    int child = 0, code = 0;

    // A process with no children at all. The distinction this asserts is
    // the one a caller most easily gets wrong: INVALID is permanent,
    // where RUNNING is "ask again". An init that treated them alike
    // would either spin forever or stop reaping.
    int a = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(a > 0);
    KTEST_ASSERT(scheduler_poll_any(a, &child, &code) == SCHED_POLL_INVALID);

    // Give it one. Nothing has died, so this is a "not yet".
    int b = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(b > 0);
    scheduler_reparent(b, a);
    KTEST_ASSERT(scheduler_poll_any(a, &child, &code) == SCHED_POLL_RUNNING);

    // Now it has died, so exactly one reap succeeds and names it.
    scheduler_kill(b, 7);
    KTEST_ASSERT(scheduler_poll_any(a, &child, &code) == SCHED_POLL_EXITED);
    KTEST_ASSERT_EQ(child, b);
    KTEST_ASSERT_EQ(code, 7);

    // Reaped exactly once: the second ask is back to "no children",
    // not a second corpse.
    KTEST_ASSERT(scheduler_poll_any(a, &child, &code) == SCHED_POLL_INVALID);

    scheduler_kill(a, 0);
    scheduler_poll(a, &code);
}

KTEST("proctree", "a dead parent's children are reparented, not left pointing at it") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    int parent = scheduler_spawn(SPIN_PATH, 0);
    int child  = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(parent > 0 && child > 0);
    scheduler_reparent(child, parent);

    struct proc_info info;
    KTEST_ASSERT(scheduler_proc_info(find_slot(child), &info));
    KTEST_ASSERT_EQ(info.ppid, parent);

    // The parent dies. THE POINT: a pid is a slot index plus one and
    // slots are reused, so a child still naming its dead parent would
    // become the child of whatever process is handed that slot next --
    // and that process's waitpid(-1) would hand it somebody else's
    // corpse.
    scheduler_kill(parent, 0);

    KTEST_ASSERT(scheduler_proc_info(find_slot(child), &info));
    KTEST_ASSERT_EQ(info.ppid, 0);

    int code = 0;
    scheduler_poll(parent, &code);
    scheduler_kill(child, 0);
    scheduler_poll(child, &code);
}

KTEST("proctree", "a reused slot does not inherit the last tenant's children") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");

    // The end-to-end form of the test above, and the one that would
    // actually bite: kill a parent, let its slot be handed out again,
    // and check the new tenant has not silently adopted the orphan.
    int parent = scheduler_spawn(SPIN_PATH, 0);
    int child  = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(parent > 0 && child > 0);
    KTEST_ASSERT(scheduler_reparent(child, parent));

    int code = 0;
    scheduler_kill(parent, 0);
    KTEST_ASSERT(scheduler_poll(parent, &code) == SCHED_POLL_EXITED); // slot free

    int reused = scheduler_spawn(SPIN_PATH, 0);
    KTEST_ASSERT(reused > 0);

    // Spawn takes the LOWEST free slot, so the pid just freed is the one
    // handed out again -- which is what makes this test able to fail at
    // all. If that ever stops being true the test is measuring nothing,
    // so say so rather than passing quietly.
    if (reused != parent) {
        scheduler_kill(reused, 0);
        scheduler_poll(reused, &code);
        scheduler_kill(child, 0);
        scheduler_poll(child, &code);
        KTEST_SKIP("spawn did not reuse the freed slot -- nothing to test");
    }

    // The orphan must NOT be this process's child. Unconditional: with
    // reparenting broken the orphan still names this pid, and this
    // reports RUNNING ("you have a live child") instead of INVALID.
    int found = 0, dummy = 0;
    KTEST_ASSERT(scheduler_poll_any(reused, &found, &dummy) == SCHED_POLL_INVALID);

    scheduler_kill(reused, 0);
    scheduler_poll(reused, &code);
    scheduler_kill(child, 0);
    scheduler_poll(child, &code);
}
