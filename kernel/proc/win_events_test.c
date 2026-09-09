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
#include "string.h"
#include "win_events.h"
#include "win_server.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"

#define EVENT_PATH "/tests/event_test"
#define READY_PATH "/tests/waitready_test"
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

static struct win_event raw_mouse(int x, int y, uint32_t buttons) {
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = WIN_EV_RAW_MOUSE;
    ev.a = x; ev.b = y; ev.mods = buttons;
    return ev;
}

KTEST("win_events", "mouse motion coalesces into one slot, and a press keeps its own") {
    win_events_reset(1);
    for (int i = 0; i < 3 * WIN_EVENT_QUEUE_MAX; i++) {
        struct win_event ev = raw_mouse(i, 2 * i, 0);
        KTEST_ASSERT(win_events_push(1, &ev));
    }
    KTEST_ASSERT_EQ(win_events_pending(1), 1);
    KTEST_ASSERT_EQ(win_events_dropped(1), 0);
    struct win_event press = raw_mouse(100, 200, 1);   // button down: a new slot
    KTEST_ASSERT(win_events_push(1, &press));
    struct win_event drag = raw_mouse(101, 201, 1);    // moving with it held: merges into that slot
    KTEST_ASSERT(win_events_push(1, &drag));
    KTEST_ASSERT_EQ(win_events_pending(1), 2);
    struct win_event got;
    KTEST_ASSERT(win_events_pop(1, &got));
    KTEST_ASSERT_EQ(got.a, 3 * WIN_EVENT_QUEUE_MAX - 1);   // the newest position won
    KTEST_ASSERT(win_events_pop(1, &got));
    KTEST_ASSERT_EQ(got.a, 101); KTEST_ASSERT_EQ((int)got.mods, 1);
    win_events_reset(1);
}

KTEST("win_events", "overflow sheds input before a notification") {
    win_events_reset(1);
    struct win_event screen;
    k_memset(&screen, 0, sizeof screen);
    screen.type = WIN_EV_SCREEN; screen.a = 1280; screen.b = 1024;
    KTEST_ASSERT(win_events_push(1, &screen));
    // The queue fills with keys behind it; the notification is the
    // OLDEST event and must still not be the one dropped.
    for (int i = 0; i < WIN_EVENT_QUEUE_MAX; i++) {
        struct win_event ev = key_event(i);
        KTEST_ASSERT(win_events_push(1, &ev));
    }
    KTEST_ASSERT_EQ(win_events_pending(1), WIN_EVENT_QUEUE_MAX);
    KTEST_ASSERT_EQ(win_events_dropped(1), 1);
    struct win_event got;
    KTEST_ASSERT(win_events_pop(1, &got));
    KTEST_ASSERT_EQ((int)got.type, WIN_EV_SCREEN);
    KTEST_ASSERT_EQ(got.a, 1280);
    KTEST_ASSERT(win_events_pop(1, &got));
    KTEST_ASSERT_EQ(got.a, 1);   // key 0 was the one shed
    win_events_reset(1);
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

// SYS_WAIT_READY, the timed wait a compositor needs. Four properties,
// and the third is the one the whole call exists for: it must not
// CONSUME the event it waited for, or the caller that drains its own
// queue (userland/wm/wm_rawin.c) silently loses one per wait.
//
// The helper reports a BITMASK of its own sub-checks, so a failure names
// which property broke rather than just "one of the four".
KTEST("win_events", "SYS_WAIT_READY times out, wakes early, and consumes nothing") {
    if (!fs_exists(READY_PATH)) KTEST_SKIP("no " READY_PATH " on this boot");

    int pid = scheduler_spawn(READY_PATH, 0);
    KTEST_ASSERT(pid != 0);

    // Its first sub-check is a 100 ms wait on an EMPTY queue, so the
    // push below must not land during it -- 40 ticks leaves 30 of slack
    // for the spawn and the ELF load on top of that 10.
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 40) { }

    // Still parked in the second wait, not exited: a wait_ready that
    // never blocked at all would have run to the end by now.
    int exit_code = -1;
    KTEST_ASSERT(scheduler_poll(pid, &exit_code) == SCHED_POLL_RUNNING);

    struct win_event ev = key_event('z');
    KTEST_ASSERT(win_events_push(pid, &ev));

    int exited = 0;
    start = pit_ticks();
    while (pit_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &exit_code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }

    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(exit_code, 0xF); // a bitmask, so a failure names WHICH property
}

// win_server.c's refusal paths, which are its access-control story and
// are worth pinning down independently of anything drawing.
//
// Narrow ON PURPOSE: create, present and ownership isolation are the
// compositor's now, and are covered end to end by
// tools/winclient_test.py driving a real ring-3 client.
KTEST("win_server", "requests are refused when no server is registered") {
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
