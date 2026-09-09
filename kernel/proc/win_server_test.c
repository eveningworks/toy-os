// Tests for what the kernel still knows about a client window.
//
// **IT NO LONGER KNOWS WHERE THE PIXELS ARE.** A window buffer is a
// named shm object the client creates and grants to the compositor,
// which opens it itself (docs/winserver-ring3-design.md, stage 5b) --
// so the mapping, revocation and poisoning these tests used to walk
// page tables for are gone, and with them the argument for reaching
// into two address spaces here. What is left in ring 0 is bookkeeping:
// which slot, which buffer is front, how big each one is, and WHICH
// OBJECT is behind it. That last one is the generation, and it is the
// only thing a compositor has to be told to re-open a name.
//
// So these drive win_server_request() the way a client does and read
// the events a compositor would get, with win_events_pop(). Asserting
// on the EVENT rather than on the internal field is the point: the
// event is the contract, and the packing (WIN_PRESENT_B) is a place the
// two sides can disagree.
//
// POSITIVE CONTROL: drop the `wb->gen++` from rebuild_buffer(). Exactly
// the two generation checks go red ("a present carries the front
// buffer's generation" and "a resize bumps only the buffer it
// rebuilds"), on the event's own value. Re-run it after any change
// here; a clean run of tests that cannot fail is worth nothing.
//
// WHAT THESE CANNOT PROVE, stated rather than implied: that the
// compositor's mapping of a client's object is correct. That is two
// ring-3 processes and an shm grant, with no kernel state in the middle
// -- tools/window_resize_probe.py and the GUI suite are where it is
// checked.
#include "ktest.h"
#include "win_server.h"
#include "win_events.h"
#include "win_proto.h"
#include "scheduler.h"
#include <stddef.h>

// The pids are CHOSEN AT RUN TIME, not named. Every window-server pid
// is one a real process can hold -- WIN_SERVER_MAX_PIDS is
// SCHED_MAX_PROCS -- so there is no synthetic number that is safe, and
// pid 3 (what this file used to hardcode) is what toywm gets on an
// ordinary boot: the fixture was creating and destroying windows on the
// live desktop's own list.
static int spare_pids(int *client, int *comp) {
    *client = *comp = 0;
    for (int p = SCHED_MAX_PROCS - 1; p > 0; p--) {
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

KTEST("winshare", "claiming needs no registered presentation layer") {
    // THE stage-4 property: the ring-3 WM is itself the compositor, so
    // there is no kernel-side presentation layer for it to wait on. Every
    // other request is refused outright when none is registered; this one
    // must not be. Skipped when the desktop is up, since the condition
    // under test is then not present -- `make test` runs before GUI mode,
    // which is when this actually means something.
    // EITHER KIND OF SERVER counts. This guard read `win_server_active()`
    // alone, which answers "is a RING-0 presentation layer registered" --
    // and the desktop stopped being one when it became a ring-3
    // compositor, so the skip silently stopped firing while its comment
    // went on claiming it did. Nothing noticed until init started the
    // desktop at boot (docs/init-design.md stage 2) and `make test`
    // finally ran with one up.
    if (win_server_any()) KTEST_SKIP("desktop is up -- no !g_ops regime to test");

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

