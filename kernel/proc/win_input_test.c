// Tests for the compositor's input path: the queue (win_input.c) and,
// more importantly, the blocking wait built on
// scheduler_block_current()/scheduler_wake().
//
// The blocking half is the part worth testing hard. A blocking syscall
// in this kernel cannot wait in place -- that was tried and hangs after
// one event, because g_next_kernel_rsp isn't reentrant (see idt.h's
// isr_in_progress()) -- so it deschedules instead, and "descheduled,
// then woken, then resumed with the right value in RAX" is a chain with
// several places to get it silently wrong. The end-to-end tests below
// drive a real ring-3 process through all of it and check the one
// thing that can only be true if every link worked: the exit code.
//
// **THE QUEUE IS THE COMPOSITOR'S**, so every test here holds the role
// for its duration and skips while a desktop does. tools/ktest_run.py
// frees the role before the suite, which is how the gate still runs
// them.
#include "ktest.h"
#include "string.h"
#include "win_input.h"
#include "win_role.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"
#include "klog.h"
#include "kfmt.h"   // klog_printf
#include "process.h"
#include "proc_info.h"
#include "keyboard.h"
#include "input.h"   // input_report_key(): a key, as a driver reports one

#define EVENT_PATH "/tests/event_test"
#define READY_PATH "/tests/waitready_test"
#define TIMEOUT_TICKS 500 // ~5s; a blocked-forever process must fail, not hang

#define SKIP_IF_ROLE_HELD                                                     \
    do {                                                                      \
        if (win_server_compositor_pid())                                      \
            KTEST_SKIP("a compositor holds the role");                        \
    } while (0)

// A pid no process holds, to give the role to for a queue-only test.
static int spare_pid(void) {
    for (int p = SCHED_PID_MAX - 1; p > 0; p--)
        if (!scheduler_pid_valid(p)) return p;
    return 0;
}

static struct win_event key_event(int code) {
    struct win_event ev = {0};
    ev.type = WIN_EV_RAW_KEY;
    ev.a = code;
    return ev;
}

static struct win_event raw_mouse(int x, int y, uint32_t buttons) {
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = WIN_EV_RAW_MOUSE;
    ev.a = x; ev.b = y; ev.mods = buttons;
    return ev;
}

KTEST("win_input", "queue delivers in order and reports pending") {
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    win_server_set_compositor(pid, 0);   // resets the queue
    KTEST_ASSERT_EQ(win_input_pending(), 0);

    struct win_event a = key_event('a'), b = key_event('b');
    KTEST_ASSERT(win_input_push(&a));
    KTEST_ASSERT(win_input_push(&b));
    KTEST_ASSERT_EQ(win_input_pending(), 2);

    struct win_event got;
    KTEST_ASSERT(win_input_pop(&got));
    KTEST_ASSERT_EQ(got.a, 'a'); // FIFO, not LIFO
    KTEST_ASSERT(win_input_pop(&got));
    KTEST_ASSERT_EQ(got.a, 'b');
    KTEST_ASSERT_EQ(win_input_pending(), 0);
    KTEST_ASSERT(!win_input_pop(&got)); // empty
    win_server_set_compositor(0, 0);
}

KTEST("win_input", "with no compositor nothing is queued") {
    SKIP_IF_ROLE_HELD;
    struct win_event ev = key_event('x'), got;
    KTEST_ASSERT(!win_input_push(&ev));
    KTEST_ASSERT(!win_input_pop(&got));
    KTEST_ASSERT_EQ(win_input_pending(), 0);
}

KTEST("win_input", "overflow drops the oldest, not the newest") {
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    win_server_set_compositor(pid, 0);

    // One more than fits, so exactly one drop, and every event
    // distinguishable by its `a` field.
    for (int i = 0; i < WIN_EVENT_QUEUE_MAX + 1; i++) {
        struct win_event ev = key_event(i);
        KTEST_ASSERT(win_input_push(&ev));
    }
    KTEST_ASSERT_EQ(win_input_pending(), WIN_EVENT_QUEUE_MAX);
    KTEST_ASSERT_EQ(win_input_dropped(), 1);

    // Event 0 is the one that should be gone -- the queue must now
    // start at 1 and end at the NEWEST event.
    struct win_event got;
    KTEST_ASSERT(win_input_pop(&got));
    KTEST_ASSERT_EQ(got.a, 1);
    for (int i = 2; i <= WIN_EVENT_QUEUE_MAX; i++) {
        KTEST_ASSERT(win_input_pop(&got));
        KTEST_ASSERT_EQ(got.a, i);
    }
    KTEST_ASSERT_EQ(win_input_pending(), 0);

    win_server_set_compositor(0, 0);
    KTEST_ASSERT_EQ(win_input_dropped(), 0); // the role change clears the counter too
}

KTEST("win_input", "mouse motion coalesces into one slot, and a press keeps its own") {
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    win_server_set_compositor(pid, 0);
    for (int i = 0; i < 3 * WIN_EVENT_QUEUE_MAX; i++) {
        struct win_event ev = raw_mouse(i, 2 * i, 0);
        KTEST_ASSERT(win_input_push(&ev));
    }
    KTEST_ASSERT_EQ(win_input_pending(), 1);
    KTEST_ASSERT_EQ(win_input_dropped(), 0);
    struct win_event press = raw_mouse(100, 200, 1);   // button down: a new slot
    KTEST_ASSERT(win_input_push(&press));
    struct win_event drag = raw_mouse(101, 201, 1);    // moving with it held: merges into that slot
    KTEST_ASSERT(win_input_push(&drag));
    KTEST_ASSERT_EQ(win_input_pending(), 2);
    struct win_event got;
    KTEST_ASSERT(win_input_pop(&got));
    KTEST_ASSERT_EQ(got.a, 3 * WIN_EVENT_QUEUE_MAX - 1);   // the newest position won
    KTEST_ASSERT(win_input_pop(&got));
    KTEST_ASSERT_EQ(got.a, 101); KTEST_ASSERT_EQ((int)got.mods, 1);
    win_server_set_compositor(0, 0);
}

KTEST("win_input", "overflow sheds input before a notification") {
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    win_server_set_compositor(pid, 0);
    struct win_event screen;
    k_memset(&screen, 0, sizeof screen);
    screen.type = WIN_EV_SCREEN; screen.a = 1280; screen.b = 1024;
    KTEST_ASSERT(win_input_push(&screen));
    // The input queue fills with keys behind it; the notification has
    // a queue of its own, so it is neither shed nor counted against them.
    for (int i = 0; i < WIN_EVENT_QUEUE_MAX; i++) {
        struct win_event ev = key_event(i);
        KTEST_ASSERT(win_input_push(&ev));
    }
    KTEST_ASSERT_EQ(win_input_pending(), WIN_EVENT_QUEUE_MAX + 1);
    KTEST_ASSERT_EQ(win_input_dropped(), 0);
    struct win_event got;
    KTEST_ASSERT(win_input_pop(&got));
    KTEST_ASSERT_EQ((int)got.type, WIN_EV_SCREEN);
    KTEST_ASSERT_EQ(got.a, 1280);
    KTEST_ASSERT(win_input_pop(&got));
    KTEST_ASSERT_EQ(got.a, 0);   // nothing shed
    win_server_set_compositor(0, 0);
}

KTEST("win_input", "a role change empties the queue") {
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    win_server_set_compositor(pid, 0);
    struct win_event ev = key_event('q');
    KTEST_ASSERT(win_input_push(&ev));
    // A successor must not inherit a predecessor's keystrokes.
    win_server_set_compositor(pid - 1 > 0 ? pid - 1 : pid, 0);
    KTEST_ASSERT_EQ(win_input_pending(), 0);
    win_server_set_compositor(0, 0);
}

// **A TIMEOUT MUST SAY WHAT IT SAW.** "exited: false" names nothing --
// a helper that never parked, one parked on the wrong channel and one
// that was never scheduled all produce it, and telling them apart is
// the whole question when the syscall gate changes.
static void report_stuck(int pid, int sampled_blocked, int samples) {
    struct proc_info pi;
    int found = 0;
    for (int i = 0; i < scheduler_slot_end(); i++) {
        if (scheduler_proc_info(i, &pi) == 1 && pi.pid == pid) { found = 1; break; }
    }
    if (!found) { klog_printf("win_input: pid %d has no slot at all\n", pid); return; }
    // scheduler_current_pid() is read from KERNEL context, so anything
    // but 0 means current_index names a process while the kernel is the
    // thing executing -- a switch recorded in procs[] that the CPU never
    // performed.
    klog_printf("win_input: pid %d stuck -- state=%u wait=%u cpu_ns=%llu "
                "blocked in %d of %d samples, current_pid=%d\n",
                pid, pi.state, pi.wait_reason, (unsigned long long)pi.cpu_ns,
                sampled_blocked, samples, scheduler_current_pid());
    // A tick early-returns while either of these holds, so a process
    // left RUNNING is never put back to READY and never picked again.
    klog_printf("win_input: preempt_depth=%d armed=%d\n",
                scheduler_preempt_depth(), process_context_is_armed());
    scheduler_trace_dump();
}

// The end-to-end one: a real ring-3 process, holding the role, blocks
// in SYS_WAIT_EVENT, gets woken by events pushed from kernel code, and
// exits with the count it received. Nothing short of the whole chain
// working produces the right exit code -- if the block silently failed,
// the syscall would return -1 and the process would exit early with a
// smaller count; if the wake never landed, it would still be blocked at
// the timeout.
KTEST("win_input", "a ring-3 process blocks in SYS_WAIT_EVENT and is woken") {
    SKIP_IF_ROLE_HELD;
    if (!fs_exists(EVENT_PATH)) KTEST_SKIP("no " EVENT_PATH " on this boot");

    const int WANT = 3;
    int pid = scheduler_spawn(EVENT_PATH, "3");
    KTEST_ASSERT(pid != 0);
    // THE ROLE, or the queue is not its to wait on. The exit path drops
    // it again (win_server_client_gone).
    win_server_set_compositor(pid, scheduler_pid_pml4(pid));

    // Let it reach its first SYS_WAIT_EVENT and park. Pushing before it
    // blocks would still work (the events just queue up), but then this
    // wouldn't be testing the blocking path at all.
    uint64_t start = coarse_ticks();
    while (coarse_ticks() - start < 25) { }

    // Still alive, and holding no CPU: it must be parked, not spinning
    // and not exited.
    int exit_code = -1;
    KTEST_ASSERT(scheduler_poll(pid, &exit_code) == SCHED_POLL_RUNNING);

    // One at a time, with a gap, so each push has to wake it
    // individually -- a single batch could be satisfied by one wake and
    // would hide a wake that only ever fires once.
    for (int i = 0; i < WANT; i++) {
        struct win_event ev = key_event('a' + i);
        KTEST_ASSERT(win_input_push(&ev));
        uint64_t t = coarse_ticks();
        while (coarse_ticks() - t < 10) { }
    }

    int exited = 0, samples = 0, blocked = 0;
    start = coarse_ticks();
    while (coarse_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &exit_code) == SCHED_POLL_EXITED) { exited = 1; break; }
        struct proc_info pi;
        for (int i = 0; i < scheduler_slot_end(); i++) {
            if (scheduler_proc_info(i, &pi) == 1 && pi.pid == pid) {
                samples++;
                if (pi.state == PROC_STATE_BLOCKED) blocked++;
                break;
            }
        }
    }
    if (!exited) report_stuck(pid, blocked, samples);

    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(exit_code, WANT); // it received every event, and only those
    KTEST_ASSERT_EQ(win_server_compositor_pid(), 0); // and the role went with it
}

// SYS_WAIT_READY, the timed wait a compositor needs. Four properties,
// and the third is the one the whole call exists for: it must not
// CONSUME the event it waited for, or the caller that drains its own
// queue (userland/wm/wm_rawin.c) silently loses one per wait.
//
// The helper reports a BITMASK of its own sub-checks, so a failure names
// which property broke rather than just "one of the four".
KTEST("win_input", "SYS_WAIT_READY times out, wakes early, and consumes nothing") {
    SKIP_IF_ROLE_HELD;
    if (!fs_exists(READY_PATH)) KTEST_SKIP("no " READY_PATH " on this boot");

    int pid = scheduler_spawn(READY_PATH, 0);
    KTEST_ASSERT(pid != 0);
    win_server_set_compositor(pid, scheduler_pid_pml4(pid));

    // Its first sub-check is a 100 ms wait on an EMPTY queue, so the
    // push below must not land during it -- 40 ticks leaves 30 of slack
    // for the spawn and the ELF load on top of that 10.
    uint64_t start = coarse_ticks();
    while (coarse_ticks() - start < 40) { }

    // Still parked in the second wait, not exited: a wait_ready that
    // never blocked at all would have run to the end by now.
    int exit_code = -1;
    KTEST_ASSERT(scheduler_poll(pid, &exit_code) == SCHED_POLL_RUNNING);

    struct win_event ev = key_event('z');
    KTEST_ASSERT(win_input_push(&ev));

    int exited = 0;
    start = coarse_ticks();
    while (coarse_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &exit_code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }

    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(exit_code, 0xF); // a bitmask, so a failure names WHICH property
}

// A process that is NOT the compositor gets -EPERM, not a queue: a
// client's events are on its own ring, and a stale binary asking here
// must find out rather than park forever.
KTEST("win_input", "only the compositor may wait for kernel events") {
    SKIP_IF_ROLE_HELD;
    if (!fs_exists(EVENT_PATH)) KTEST_SKIP("no " EVENT_PATH " on this boot");
    int pid = scheduler_spawn(EVENT_PATH, "1");
    KTEST_ASSERT(pid != 0);
    // No role given: its first SYS_WAIT_EVENT is refused, and it exits
    // with the count it had -- zero.
    int exit_code = -1, exited = 0;
    uint64_t start = coarse_ticks();
    while (coarse_ticks() - start < TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &exit_code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    KTEST_ASSERT_EQ(exit_code, 0);
}

KTEST("win_input", "a compositor change empties the keyboard's stream and its owed releases") {
    // THROUGH THE ROLE, not the keyboard's own call: a restart or a
    // handoff must not hand the new compositor the old one's Enter, nor
    // the release of a key it never saw go down.
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    scheduler_preempt_disable();
    win_server_set_compositor(pid, 0);
    input_report_key(INPUT_KEY_ENTER, 1);        // seen by the first compositor
    int queued = 0, c, d;
    uint8_t m;
    win_server_set_compositor(pid, 0);           // ...the role changes hands
    input_report_key(INPUT_KEY_ENTER, 0);        // its release arrives after
    while (keyboard_try_get_key(&c, &d, &m)) queued++;
    win_server_set_compositor(0, 0);
    scheduler_preempt_enable();
    KTEST_ASSERT_EQ(queued, 0);
}

// These record what they see and restore the role BEFORE asserting:
// a failed assertion returns from the test, and a stand-in pid left
// holding the role would make every later test skip and keep keys from
// the console.

KTEST("win_input", "a full queue of releases sheds nothing, and notices still arrive") {
    // A release is never evicted: with nothing else to shed, a new INPUT
    // event is refused. A notice has its own queue, so input pressure
    // cannot cost one -- six window-lifecycle notices over a full queue
    // all arrive, first, and in order.
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    win_server_set_compositor(pid, 0);
    int pushed_ups = 0, took_press, took_notices = 0, ups = 0, notices_first = 1;
    int seen_notices = 0, in_order = 1;
    for (int i = 0; i < WIN_EVENT_QUEUE_MAX; i++) {
        struct win_event up = { .type = WIN_EV_RAW_KEY_UP, .a = 'a' + i % 26 };
        pushed_ups += win_input_push(&up);
    }
    struct win_event press = { .type = WIN_EV_RAW_KEY, .a = 'z' };
    took_press = win_input_push(&press);
    for (int i = 0; i < 6; i++) {
        struct win_event gone = { .type = WIN_EV_CLOSE, .a = 100 + i };
        took_notices += win_input_push(&gone);
    }
    struct win_event got;
    while (win_input_pop(&got)) {
        if (got.type == WIN_EV_RAW_KEY_UP) ups++;
        if (got.type == WIN_EV_CLOSE) {
            if (ups) notices_first = 0;
            if (got.a != 100 + seen_notices) in_order = 0;
            seen_notices++;
        }
    }
    win_server_set_compositor(0, 0);
    KTEST_ASSERT_EQ(pushed_ups, WIN_EVENT_QUEUE_MAX);
    KTEST_ASSERT(!took_press);
    KTEST_ASSERT_EQ(ups, WIN_EVENT_QUEUE_MAX);   // every release survived
    KTEST_ASSERT_EQ(took_notices, 6);
    KTEST_ASSERT_EQ(seen_notices, 6);
    KTEST_ASSERT(notices_first);
    KTEST_ASSERT(in_order);
}

KTEST("win_input", "a coalesced notice moves to the END, behind what came after it") {
    // SCREEN, then a lifecycle notice, then a newer SCREEN: the compositor
    // must see the lifecycle notice before the new mode, not after it.
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    win_server_set_compositor(pid, 0);
    struct win_event scr = { .type = WIN_EV_SCREEN, .a = 640, .b = 480 };
    struct win_event gone = { .type = WIN_EV_CLOSE, .a = 7 };
    win_input_push(&scr);
    win_input_push(&gone);
    scr.a = 800;
    win_input_push(&scr);
    int pending = win_input_pending();
    struct win_event a = { 0 }, b = { 0 };
    win_input_pop(&a);
    win_input_pop(&b);
    win_server_set_compositor(0, 0);
    KTEST_ASSERT_EQ(pending, 2);                  // the older SCREEN is gone
    KTEST_ASSERT_EQ((int)a.type, WIN_EV_CLOSE);
    KTEST_ASSERT_EQ((int)b.type, WIN_EV_SCREEN);
    KTEST_ASSERT_EQ(b.a, 800);
}

KTEST("win_input", "with one free slot per poll, keys and positions both drain") {
    // THE REAL DRAIN (win_input_drain_keys()), one slot at a time: which
    // stream goes first must alternate per poll, or one starves.
    SKIP_IF_ROLE_HELD;
    int pid = spare_pid();
    if (!pid) KTEST_SKIP("no spare pid");
    scheduler_preempt_disable();
    win_server_set_compositor(pid, 0);
    for (int i = 0; i < WIN_EVENT_QUEUE_MAX - 1; i++) {
        struct win_event filler = { .type = WIN_EV_RAW_KEY, .a = 0x7F };
        win_input_push(&filler);
    }
    for (int i = 0; i < 3; i++) {                     // both streams backlogged
        input_report_key(INPUT_KEY_A, 1);
        input_report_key(INPUT_KEY_A, 0);
    }
    struct win_event got;
    for (int round = 0; round < 6; round++) {
        win_input_pop(&got);                          // the compositor takes one...
        win_input_drain_keys();                       // ...and a poll refills one
    }
    int keys = 0, phys = 0;
    while (win_input_pop(&got)) {
        if (got.type == WIN_EV_RAW_KEY_PHYS) phys++;
        else if ((got.type == WIN_EV_RAW_KEY && got.a != 0x7F) || got.type == WIN_EV_RAW_KEY_UP) keys++;
    }
    win_server_set_compositor(0, 0);
    scheduler_preempt_enable();
    KTEST_ASSERT(keys >= 2);
    KTEST_ASSERT(phys >= 2);
}
