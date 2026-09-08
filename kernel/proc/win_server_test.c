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

#define WIN_W 64
#define WIN_H 32

// A fixture: a client pid with one window, and a compositor pid whose
// event queue the assertions read. No address spaces: nothing here maps
// anything any more.
struct fixture {
    uint32_t id;
    int client_pid;
    int comp_pid;
};

static int fixture_up(struct fixture *f) {
    if (!spare_pids(&f->client_pid, &f->comp_pid)) return 0;
    if (!win_server_set_compositor(f->comp_pid, 0)) return 0;
    if (!win_server_create_raw(f->client_pid, WIN_W, WIN_H, &f->id)) return 0;
    return 1;
}

static void fixture_down(struct fixture *f) {
    if (f->client_pid) win_server_destroy_raw(f->client_pid, f->id);
    // Safe to clear unconditionally only because SKIP_IF_ROLE_HELD
    // proved the role was free before the fixture took it.
    win_server_set_compositor(0, 0);
    win_events_reset(f->comp_pid);
}

// **A FAILING ASSERT RETURNS FROM THE TEST**, so nothing after it runs
// -- including the teardown that gives the compositor role back. One
// red test would then skip every later one in this file with "a
// compositor holds the role", turning one failure into a silent eight.
// (Found by running this file's own positive control.) These tear the
// fixture down first, and evaluate the value ONCE into a temporary, so
// a request under test is never issued twice.
#define FIX_ASSERT(f, cond)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fixture_down(f);                                                  \
            ktest_fail(ctx, #cond, __FILE__, __LINE__);                       \
            return;                                                           \
        }                                                                     \
    } while (0)

#define FIX_ASSERT_EQ(f, got, expected)                                       \
    do {                                                                      \
        int64_t fix_g_ = (int64_t)(got);                                      \
        int64_t fix_e_ = (int64_t)(expected);                                 \
        if (fix_g_ != fix_e_) {                                               \
            fixture_down(f);                                                  \
            ktest_fail_eq(ctx, #got, fix_g_, fix_e_, __FILE__, __LINE__);     \
            return;                                                           \
        }                                                                     \
    } while (0)

// Drains the compositor's queue down to the newest event of `type`.
static int last_event(int pid, uint32_t type, struct win_event *out) {
    struct win_event ev;
    int got = 0;
    while (win_events_pop(pid, &ev)) {
        if (ev.type != type) continue;
        *out = ev;
        got = 1;
    }
    return got;
}

KTEST("winshare", "a window is created at the size the client claims") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("no unused pids"); }

    // BOTH buffers, because the client made both before it asked. The
    // kernel takes its word for it: it holds neither object, so there
    // is nothing to look at -- the compositor checks the size against
    // the mapping it actually made, which is the only side that can.
    FIX_ASSERT_EQ(&f, win_server_buf_size(f.client_pid, f.id, 0), WIN_W * WIN_H);
    FIX_ASSERT_EQ(&f, win_server_buf_size(f.client_pid, f.id, 1), WIN_W * WIN_H);
    FIX_ASSERT_EQ(&f, win_server_window_count(f.client_pid), 1);

    struct win_event ev;
    FIX_ASSERT(&f, last_event(f.comp_pid, WIN_EV_CLIENT_CREATED, &ev));
    FIX_ASSERT_EQ(&f, ev.a, f.client_pid);
    FIX_ASSERT_EQ(&f, ev.b, WIN_W);
    FIX_ASSERT_EQ(&f, (int)ev.mods, WIN_H);

    fixture_down(&f);
}

KTEST("winshare", "the client's proposed slot is honoured, and a taken one refused") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!spare_pids(&f.client_pid, &f.comp_pid)) KTEST_SKIP("no unused pids");
    if (!win_server_set_compositor(f.comp_pid, 0)) KTEST_SKIP("cannot take the role");

    // THE CLIENT PICKS THE SLOT because its buffer objects are already
    // named after it (abi/win_proto.h's WIN_BUF_NAME_FMT). A slot the
    // kernel chose instead would name objects that do not exist.
    struct win_request_msg req = {0};
    req.type = WIN_REQ_CREATE;
    req.window = 2;
    req.a = WIN_W; req.b = WIN_H;
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &req), 1);
    f.id = req.window;   // so a failure below still tears it down
    FIX_ASSERT_EQ(&f, f.id, 2u);
    // The same slot again is REFUSED rather than silently moved: a
    // client whose window landed somewhere else would go on drawing
    // into the object it named after the slot it asked for.
    struct win_request_msg again = {0};
    again.type = WIN_REQ_CREATE;
    again.window = 2;
    again.a = WIN_W; again.b = WIN_H;
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &again), 0);

    fixture_down(&f);
}
KTEST("winshare", "a present flips the buffer, so the two never collide") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("no unused pids"); }

    struct win_request_msg req = {0};
    req.type = WIN_REQ_PRESENT;
    req.window = f.id;

    // The return is the new FRONT index biased by one, so 0 can still
    // mean refused -- and it must ALTERNATE, which is the whole of
    // double buffering: the client draws into whichever this does not
    // name.
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &req), 2); // front 1
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &req), 1); // front 0

    struct win_event ev;
    FIX_ASSERT(&f, last_event(f.comp_pid, WIN_EV_CLIENT_PRESENT, &ev));
    FIX_ASSERT_EQ(&f, WIN_PRESENT_BUF(ev.b), 0);
    // THE SIZE TRAVELS WITH THE FRAME, packed beside it.
    FIX_ASSERT_EQ(&f, WIN_PRESENT_W(ev.mods), WIN_W);
    FIX_ASSERT_EQ(&f, WIN_PRESENT_H(ev.mods), WIN_H);

    fixture_down(&f);
}

KTEST("winshare", "a present carries the front buffer's generation") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("no unused pids"); }

    // A fresh window's objects are the first ones under their names.
    FIX_ASSERT_EQ(&f, win_server_buf_gen(f.client_pid, f.id, 0), 0u);

    // The client replaced buffer 0's object -- unlinked it and made a
    // new one under the same name. Only it can, so only it can say so.
    struct win_request_msg buf = {0};
    buf.type = WIN_REQ_BUFFER;
    buf.window = f.id;
    buf.a = 0;
    buf.b = WIN_W * 2; buf.c = WIN_H * 2;
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &buf), 1);
    FIX_ASSERT_EQ(&f, win_server_buf_gen(f.client_pid, f.id, 0), 1u);
    FIX_ASSERT_EQ(&f, win_server_buf_gen(f.client_pid, f.id, 1), 0u);

    // **THE SAME SIZE STILL COUNTS.** A client re-creating a buffer at
    // the size it already had makes a NEW object under the same name,
    // and a kernel that inferred "replaced" from the dimensions would
    // leave the compositor mapping memory nobody draws into -- a window
    // frozen on its last frame. Reachable from a drag that proposes the
    // size a window already has.
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &buf), 1);
    FIX_ASSERT_EQ(&f, win_server_buf_gen(f.client_pid, f.id, 0), 2u);

    // Two presents to bring buffer 0 back to the front, then read what
    // the compositor was actually told -- the packing is a place the
    // two sides can disagree, so the assertion is on the event.
    struct win_request_msg req = {0};
    req.type = WIN_REQ_PRESENT;
    req.window = f.id;
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &req), 2);
    FIX_ASSERT_EQ(&f, win_server_request(f.client_pid, &req), 1);

    struct win_event ev;
    FIX_ASSERT(&f, last_event(f.comp_pid, WIN_EV_CLIENT_PRESENT, &ev));
    FIX_ASSERT_EQ(&f, WIN_PRESENT_BUF(ev.b), 0);
    FIX_ASSERT_EQ(&f, WIN_PRESENT_GEN(ev.b), 2u);
    FIX_ASSERT_EQ(&f, WIN_PRESENT_W(ev.mods), WIN_W * 2);

    fixture_down(&f);
}

KTEST("winshare", "a resize rebuilds only the buffer the client names") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("no unused pids"); }

    // The FRONT buffer still holds the last finished frame at the size
    // it was drawn at, and a resize must leave it alone -- rebuilding
    // it is the window of black the configure/ack handshake exists to
    // avoid. win_server_resize_raw() picks the back one, as a client
    // with nothing in flight would.
    int front = 0;
    FIX_ASSERT(&f, win_server_resize_raw(f.client_pid, f.id, WIN_W * 3, WIN_H * 3));

    FIX_ASSERT_EQ(&f, win_server_buf_size(f.client_pid, f.id, front ^ 1),
                    WIN_W * 3 * WIN_H * 3);
    FIX_ASSERT_EQ(&f, win_server_buf_size(f.client_pid, f.id, front), WIN_W * WIN_H);
    // And the generation went up on that one ALONE: a compositor
    // re-opening the front buffer's name here would drop the frame it
    // is showing for a new, empty object.
    FIX_ASSERT_EQ(&f, win_server_buf_gen(f.client_pid, f.id, front ^ 1), 1u);
    FIX_ASSERT_EQ(&f, win_server_buf_gen(f.client_pid, f.id, front), 0u);

    struct win_event ev;
    FIX_ASSERT(&f, last_event(f.comp_pid, WIN_EV_CLIENT_RESIZED, &ev));
    FIX_ASSERT_EQ(&f, ev.b, WIN_W * 3);

    fixture_down(&f);
}

KTEST("winshare", "a destroyed window frees its slot, and the client's death takes the rest") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("no unused pids"); }

    FIX_ASSERT(&f, win_server_destroy_raw(f.client_pid, f.id));
    FIX_ASSERT_EQ(&f, win_server_window_count(f.client_pid), 0);

    struct win_event ev;
    FIX_ASSERT(&f, last_event(f.comp_pid, WIN_EV_CLIENT_DESTROYED, &ev));

    // The SAME slot is immediately reusable. It used to be RETIRED --
    // held until the compositor released its mapping -- which is a
    // state that cannot exist now that the mapping is the compositor's
    // own: the object stays alive under it by reference, whatever the
    // window table does.
    uint32_t again = 0;
    FIX_ASSERT(&f, win_server_create_raw(f.client_pid, WIN_W, WIN_H, &again));
    FIX_ASSERT_EQ(&f, again, f.id);

    // And a client dying takes its windows with it.
    win_server_client_gone(f.client_pid);
    FIX_ASSERT_EQ(&f, win_server_window_count(f.client_pid), 0);

    f.client_pid = 0;   // nothing left for fixture_down() to destroy
    fixture_down(&f);
}

// --- the way IN to all of the above (M41 stage 2) ---------------------
//
// Everything above calls win_server_set_compositor() directly, which
// nothing outside this file could do: the registration existed with no
// syscall and no message reaching it. These cover the message that fills
// that gap, and the two rules around it.
//
// They deliberately do NOT go through the syscall: what is interesting
// here is win_server_request()'s own handling, and driving it directly
// keeps the test free of a user address space it does not need.

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

