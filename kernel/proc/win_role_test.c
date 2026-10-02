// Tests for the compositor role (win_role.c).
//
// What is left to test here is the ROLE: who may claim it, who may
// release it, and that nothing else is served without it. Everything
// about a window is the compositor's and is checked from ring 3
// (tools/winclient_test.py, tools/uapp_test.py, tools/resize_stride_test.py).
#include "ktest.h"
#include "win_role.h"
#include "win_input.h"
#include "win_proto.h"
#include "scheduler.h"
#include <stddef.h>

// The pids are CHOSEN AT RUN TIME, not named: any number below
// SCHED_PID_MAX may be a real process's, and pid 3 (what this file used to hardcode) is what toywm gets on an
// ordinary boot: the fixture was creating and destroying windows on the
// live desktop's own list.
static int spare_pids(int *client, int *comp) {
    *client = *comp = 0;
    for (int p = SCHED_PID_MAX - 1; p > 0; p--) {
        if (scheduler_pid_valid(p)) continue; // valid counts a zombie, which is what we want
        if (!*client) { *client = p; continue; }
        *comp = p;
        return 1;
    }
    return 0;
}

// THE COMPOSITOR ROLE IS ONE GLOBAL, SHARED WITH THE LIVE DESKTOP.
// Taking it revokes that desktop's framebuffer grant (and, on the way
// out, asks its clients to close). Neither is undoable by restoring the
// role afterwards, because the damage is done on the way IN. So these
// refuse to run while anyone holds it, and tools/ktest_run.py frees the
// role before the suite -- which is how the gate still exercises them
// without the default graphical boot destroying itself.
#define SKIP_IF_ROLE_HELD                                                     \
    do {                                                                      \
        if (win_server_compositor_pid())                                      \
            KTEST_SKIP("a compositor holds the role");                        \
    } while (0)

// **THE SIX WINDOW TESTS THAT WERE HERE ARE GONE WITH THE TABLE**
// (stage 6b): a window created at the size the client claims, a
// proposed slot honoured and a taken one refused, a present flipping
// the buffer, a present carrying the front buffer's generation, a
// resize rebuilding only the buffer the client names, and a destroyed
// window freeing its slot. Every one of those is the COMPOSITOR's
// behaviour now, in ring 3, and the kernel has nothing to make a window
// out of.
//
// Where that coverage went, named so it can be checked rather than
// assumed: tools/winclient_test.py (a window is created and drawn),
// tools/uapp_test.py (the resize handshake), tools/resize_stride_test.py
// (a present carries its own geometry, and only the back buffer is
// rebuilt), tools/single_instance_test.py (slot reuse across a second
// launch) and tools/compositor_death_test.py (a client's windows going
// when it does). That is a WEAKER position than a KTEST: those run in a
// booted desktop and take seconds each, where these ran in the kernel
// in microseconds. It is the price of the subject leaving ring 0, and
// it is stated rather than glossed.

KTEST("winshare", "WIN_REQ_SET_COMPOSITOR claims and releases the role") {
    struct win_request_msg req = {0};
    int cpid, other;
    SKIP_IF_ROLE_HELD;
    if (!spare_pids(&cpid, &other)) KTEST_SKIP("no unused pids");

    // Preemption off for the body -- see the sibling test below
    // ("only the holder may release the compositor role") for the full
    // reasoning. Short version: the compositor role is a single global,
    // so anything else claiming it mid-body makes "after releasing,
    // nobody holds it" read somebody else's pid rather than 0. The skip
    // above means this no longer EVICTS a live desktop to run; it still
    // has to keep the role to itself while it runs.
    scheduler_preempt_disable();

    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), cpid);

    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), 0);

    scheduler_preempt_enable();
}

KTEST("winshare", "only the holder may release the compositor role") {
    struct win_request_msg req = {0};
    int cpid, other;
    SKIP_IF_ROLE_HELD;
    if (!spare_pids(&cpid, &other)) KTEST_SKIP("no unused pids");
    req.type = WIN_REQ_SET_COMPOSITOR;

    // PREEMPTION OFF FOR THE WHOLE BODY, and this is a precondition
    // rather than tidiness. The compositor role is a single global, so
    // anything that claims it while this runs makes the closing
    // assertion -- "after the holder releases it, nobody holds it" --
    // read that claimant's pid instead of 0. It used to race the live
    // desktop, which this test EVICTED to run at all; the skip above
    // ended the eviction, and the race is still worth closing because
    // the role is still shared.
    //
    // It passed for a long time on timing luck alone. What exposed it
    // was an unrelated change adding PCI KTESTs, whose thousands of
    // extra config-space port reads shifted boot timing enough to lose
    // the race about half the time; HEAD measured 4 runs in 4 clean and
    // the same suite with those tests present failed ~4 in 7.
    //
    // Disabling preemption ESTABLISHES the precondition instead of
    // loosening the assertion, which keeps the property under test
    // exactly as strong as it was -- the same call, and the same
    // reasoning, as the heap KTESTs that compare against a snapshot.
    scheduler_preempt_disable();

    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);

    // Without this check any process could evict the compositor, taking
    // the raw input stream and every buffer mapping down with it -- a
    // denial of service that needs no privilege at all.
    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(other, &req), 0);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), cpid);

    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), 0);

    scheduler_preempt_enable();
}

KTEST("winshare", "claiming the role needs no window server") {
    // Every other request is refused outright when no compositor holds
    // the role; SET_COMPOSITOR must not be, or nothing could ever claim
    // it. Skipped once the desktop is up, since the condition under test
    // is then not present.
    if (win_server_any()) KTEST_SKIP("desktop is up -- no unclaimed role to test");

    int cpid, other;
    if (!spare_pids(&cpid, &other)) KTEST_SKIP("no unused pids");
    (void)other;
    struct win_request_msg req = {0};
    req.type = WIN_REQ_PRESENT;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), -1); // the control

    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), cpid);

    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
}


KTEST("winshare", "requests are refused when no server is registered") {
    if (win_server_any()) KTEST_SKIP("a window server is registered (desktop is up)");

    struct win_request_msg req = {0};
    req.type = WIN_REQ_FB_MAP;
    // -1, not 0: "there is no server" is a different answer from "the
    // server said no", and a client needs to be able to tell them apart.
    KTEST_ASSERT_EQ(win_server_request(1, &req), -1);
}
