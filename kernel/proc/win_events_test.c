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
#include "win_server.h"
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

// win_server.c's refusal paths, which are its access-control story and
// are worth pinning down independently of anything drawing.
//
// Narrow ON PURPOSE: the interesting cases (create, present, ownership
// isolation between two clients) need a registered presentation layer,
// and registering a stub here would clobber the real window manager if
// this suite is ever run while the desktop is up. Those paths are
// covered end-to-end by tools/winclient_test.py instead, which drives a
// real ring-3 client against the real WM -- stronger evidence than a
// stub would give, and with nothing to clobber.
KTEST("win_server", "requests are refused when no server is registered") {
    // EITHER KIND OF SERVER counts. This guard read `win_server_active()`
    // alone, which answers "is a RING-0 presentation layer registered" --
    // and the desktop stopped being one when it became a ring-3
    // compositor, so the skip silently stopped firing while its comment
    // went on claiming it did. Nothing noticed until init started the
    // desktop at boot (docs/init-design.md stage 2) and `make test`
    // finally ran with one up.
    if (win_server_any()) KTEST_SKIP("a window server is registered (desktop is up)");

    struct win_request_msg req = {0};
    req.type = WIN_REQ_CREATE;
    req.a = 100;
    req.b = 100;
    // -1, not 0: "there is no server" is a different answer from "the
    // server said no", and a client needs to be able to tell them apart
    // -- the first means "you are not in a desktop session", the second
    // means "try something smaller".
    KTEST_ASSERT_EQ(win_server_request(1, &req), -1);
}

KTEST("win_server", "a bad pid owns nothing") {
    KTEST_ASSERT_EQ(win_server_window_count(0), 0);
    KTEST_ASSERT_EQ(win_server_window_count(-1), 0);
    KTEST_ASSERT_EQ(win_server_window_count(99), 0);
}

// --- every scheduled process has a queue ------------------------------
//
// This table read `4` long after SCHED_MAX_PROCS became 64, so a client
// in slot 4 or beyond got NULL from queue_for() and received no window
// events at all -- drawing correctly and answering nothing. The
// _Static_assert in win_events.c is the real guard; this is the runtime
// half, and it fails on the boundary rather than on an average.

KTEST("win_events", "the HIGHEST pid a scheduler can hand out has a queue") {
    // The boundary is the whole point: a table one entry short passes
    // every test written against pid 1.
    const int top = SCHED_MAX_PROCS;

    win_events_reset(top);
    KTEST_ASSERT_EQ(win_events_pending(top), 0);

    struct win_event ev = {0};
    ev.type = WIN_EV_KEY;
    ev.a = 'z';
    KTEST_ASSERT(win_events_push(top, &ev));
    KTEST_ASSERT_EQ(win_events_pending(top), 1);

    struct win_event got = {0};
    KTEST_ASSERT(win_events_pop(top, &got));
    KTEST_ASSERT_EQ(got.type, (uint32_t)WIN_EV_KEY);
    KTEST_ASSERT_EQ(got.a, 'z');

    win_events_reset(top);
}

KTEST("win_events", "a pid outside the scheduler's range has none") {
    struct win_event ev = {0};
    ev.type = WIN_EV_KEY;

    // The other half of the boundary. Refused rather than indexed, so a
    // bad pid can never reach a neighbouring process's queue.
    KTEST_ASSERT(!win_events_push(0, &ev));
    KTEST_ASSERT(!win_events_push(-1, &ev));
    KTEST_ASSERT(!win_events_push(SCHED_MAX_PROCS + 1, &ev));
    KTEST_ASSERT_EQ(win_events_pending(SCHED_MAX_PROCS + 1), 0);
}
