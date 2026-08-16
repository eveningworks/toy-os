// A ring-3 process that registers as the COMPOSITOR and logs the raw
// input stream it receives -- Milestone 41 stage 2's proof that a second
// consumer of the WM's input can exist outside the kernel.
//
// It owns no window and draws nothing. That is deliberate: what is under
// test is the pre-routing input path (WIN_EV_RAW_*) and the compositor
// registration (WIN_REQ_SET_COMPOSITOR), and a window would drag focus,
// hit-testing and z-order into a test about none of those. The real WM
// keeps routing the same events to the same windows while this runs --
// both paths are live until stage 4 deletes the old one, and
// `gui_regress.py` passing unchanged is what proves the second consumer
// changed nothing.
//
// Diagnostics go to stderr (sys_eprint), which the kernel routes into
// the kernel log and `dmesg` -- a spawned client's stdout goes into its
// parent's pipe and a windowed client's goes nowhere useful. See
// CLAUDE.md.
//
// Log grammar, one line per event, so a test asserts on text rather than
// pixels:
//   compclient: registered
//   compclient: mouse <x> <y> <buttons>
//   compclient: key <code> <mods>
//   compclient: wheel <notches>
//   compclient: released
//   compclient: exit
//
// Exits on 'q' so a test can end it without a close button to click --
// it has no window, so wm_request_close() has nothing to ask.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/stdio.h"
#include "win_proto.h"

// The one request this program makes. `a` is claim(1)/release(0); every
// other field is unused, and zeroing the whole struct rather than
// setting three fields keeps it that way if the message ever grows.
static int set_compositor(int claim) {
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((char *)&req)[i] = 0;
    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = claim;
    return sys_win_request(&req);
}

int main(void) {
    char buf[64];

    if (set_compositor(1) != 1) {
        sys_eprint("compclient: registration REFUSED\n");
        return 1;
    }
    sys_eprint("compclient: registered\n");

    for (;;) {
        struct win_event ev;
        if (sys_wait_event(&ev) != 1) continue;

        switch (ev.type) {
        case WIN_EV_RAW_MOUSE:
            snprintf(buf, sizeof buf, "compclient: mouse %d %d %u\n",
                     ev.a, ev.b, ev.mods);
            break;
        case WIN_EV_RAW_KEY:
            snprintf(buf, sizeof buf, "compclient: key %d %u\n", ev.a, ev.mods);
            break;
        case WIN_EV_RAW_WHEEL:
            snprintf(buf, sizeof buf, "compclient: wheel %d\n", ev.a);
            break;
        default:
            // Anything post-routing (a key for a window, a resize) is not
            // this program's business -- it owns no window, so receiving
            // one at all would be a bug worth seeing rather than a case
            // to handle silently.
            snprintf(buf, sizeof buf, "compclient: other %u %d %d\n",
                     ev.type, ev.a, ev.b);
            break;
        }
        sys_eprint(buf);

        // 'q' quits. Checked here rather than in the WM, so the WM's own
        // routing of that same keystroke is untouched -- the two
        // consumers are independent, which is the property on test.
        if (ev.type == WIN_EV_RAW_KEY && ev.a == 'q') break;
    }

    // Releasing explicitly rather than relying on exit, so the release
    // path (which win_server_client_gone() would otherwise cover) is
    // exercised too -- and so a `dropped` count read after this program
    // ends reflects a compositor that left cleanly.
    if (set_compositor(0) == 1) sys_eprint("compclient: released\n");
    sys_eprint("compclient: exit\n");
    return 0;
}
