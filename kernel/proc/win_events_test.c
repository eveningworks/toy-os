// Tests for the windowing protocol's delivery path: the per-process
// event queues (win_events.c) and, more importantly, the blocking wait
// built on scheduler_block_current()/scheduler_wake().
//
// The blocking half is the part worth testing hard. A blocking syscall
// in this kernel cannot wait in place -- that was tried and hangs after
// one event, because g_next_kernel_rsp isn't reentrant (see syscall.c's
// SYS_READ_KEY comment) -- so it deschedules instead, and "descheduled,
// then woken, then resumed with the right value in RAX" is a chain with
// several places to get it silently wrong. The end-to-end test below
// drives a real ring-3 process through all of it and checks the one
// thing that can only be true if every link worked: the exit code.
#include "ktest.h"
#include "win_events.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"

#define EVENT_PATH "/tests/event_test"
#define TIMEOUT_TICKS 500 // ~5s; a blocked-forever process must fail, not hang

static struct win_event key_event(int code) {
    struct win_event ev = {0};
    ev.type = WIN_EV_KEY;
    ev.a = code;
    return ev;
}

KTEST("win_events", "queue delivers in order and reports pending") {
    // pid 1's queue is only safe to poke at directly when nothing is
    // actually running as pid 1 -- true inside ktest, which runs on the
    // kernel context with an empty process table.
    win_events_reset(1);
    KTEST_ASSERT_EQ(win_events_pending(1), 0);

    struct win_event a = key_event('a'), b = key_event('b');
    KTEST_ASSERT(win_events_push(1, &a));
    KTEST_ASSERT(win_events_push(1, &b));
    KTEST_ASSERT_EQ(win_events_pending(1), 2);

    struct win_event got;
    KTEST_ASSERT(win_events_pop(1, &got));
    KTEST_ASSERT_EQ(got.a, 'a'); // FIFO, not LIFO
    KTEST_ASSERT(win_events_pop(1, &got));
    KTEST_ASSERT_EQ(got.a, 'b');
    KTEST_ASSERT_EQ(win_events_pending(1), 0);
    KTEST_ASSERT(!win_events_pop(1, &got)); // empty
}

KTEST("win_events", "overflow drops the oldest, not the newest") {
    win_events_reset(1);

    // One more than fits, so exactly one drop, and every event
    // distinguishable by its `a` field.
    for (int i = 0; i < WIN_EVENT_QUEUE_MAX + 1; i++) {
        struct win_event ev = key_event(i);
        KTEST_ASSERT(win_events_push(1, &ev));
    }
    KTEST_ASSERT_EQ(win_events_pending(1), WIN_EVENT_QUEUE_MAX);
    KTEST_ASSERT_EQ(win_events_dropped(1), 1);

    // Event 0 is the one that should be gone -- the queue must now
    // start at 1 and end at the NEWEST event, which is the whole point
    // of dropping from the old end for input.
    struct win_event got;
    KTEST_ASSERT(win_events_pop(1, &got));
    KTEST_ASSERT_EQ(got.a, 1);
    for (int i = 2; i <= WIN_EVENT_QUEUE_MAX; i++) {
        KTEST_ASSERT(win_events_pop(1, &got));
        KTEST_ASSERT_EQ(got.a, i);
    }
    KTEST_ASSERT_EQ(win_events_pending(1), 0);

    win_events_reset(1);
    KTEST_ASSERT_EQ(win_events_dropped(1), 0); // reset clears the counter too
}

KTEST("win_events", "a bad pid is refused, not written out of bounds") {
    struct win_event ev = key_event('x'), got;
    KTEST_ASSERT(!win_events_push(0, &ev));
    KTEST_ASSERT(!win_events_push(99, &ev));
    KTEST_ASSERT(!win_events_pop(0, &got));
    KTEST_ASSERT(!win_events_pop(99, &got));
    KTEST_ASSERT_EQ(win_events_pending(99), 0);
}

// The end-to-end one: a real ring-3 process blocks in SYS_WAIT_EVENT,
// gets woken by events pushed from kernel code, and exits with the
// count it received. Nothing short of the whole chain working produces
// the right exit code -- if the block silently failed, the syscall
// would return -1 and the process would exit early with a smaller
// count; if the wake never landed, it would still be blocked at the
// timeout.
KTEST("win_events", "a ring-3 process blocks in SYS_WAIT_EVENT and is woken") {
    if (!fs_exists(EVENT_PATH)) KTEST_SKIP("no " EVENT_PATH " on this boot");

    const int WANT = 3;
    int pid = scheduler_spawn(EVENT_PATH, "3");
    KTEST_ASSERT(pid != 0);

    // Let it reach its first SYS_WAIT_EVENT and park. Pushing before it
    // blocks would still work (the events just queue up), but then this
    // wouldn't be testing the blocking path at all -- it would be
    // testing the queue, which the tests above already cover.
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 25) { }

    // Still alive, and holding no CPU: it must be parked, not spinning
    // and not exited.
    int exit_code = -1;
    KTEST_ASSERT(scheduler_poll(pid, &exit_code) == SCHED_POLL_RUNNING);

    // One at a time, with a gap, so each push has to wake it
    // individually -- a single batch could be satisfied by one wake and
    // would hide a wake that only ever fires once.
    for (int i = 0; i < WANT; i++) {
        struct win_event ev = key_event('a' + i);
        KTEST_ASSERT(win_events_push(pid, &ev));
        uint64_t t = pit_ticks();
        while (pit_ticks() - t < 10) { }
    }

    int exited = 0;
    start = pit_ticks();
    while (pit_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &exit_code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }

    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(exit_code, WANT); // it received every event, and only those
}
