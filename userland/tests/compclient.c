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
#include "lib/string.h" // memset -- the request structs are zeroed
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

    // The access control, checked BEFORE registering, because after
    // registering there is no way to ask from a non-compositor without a
    // second process -- and "it was refused" is only meaningful against
    // "it was granted a moment later to the same process".
    {
        struct win_request_msg probe;
        memset(&probe, 0, sizeof probe);
        probe.type = WIN_REQ_FB_MAP;
        sys_eprint(sys_win_request(&probe) == 0
                       ? "compclient: fb-unregistered GRANTED\n"
                       : "compclient: fb-unregistered refused\n");
    }

    if (set_compositor(1) != 1) {
        sys_eprint("compclient: registration REFUSED\n");
        return 1;
    }
    sys_eprint("compclient: registered\n");

    // --- the framebuffer grant (M41 stage 4a) ---------------------
    //
    // 'f' maps the real screen and paints a marker block into it. The
    // WM is still compositing the same screen in ring 0, so the block
    // survives only until the next repaint of that region -- which is
    // exactly right for a test: it proves the mapping is live and
    // writable without this program having to own the desktop.
    struct win_request_msg fb;
    int have_fb = 0;
    unsigned fb_w = 0, fb_h = 0, fb_pitch = 0, fb_bpp = 0;

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
        // 'f' maps the framebuffer; 'p' paints and publishes a block.
        // Two keys rather than one so a test can assert the map's
        // reported geometry BEFORE anything is drawn -- a single key
        // would make "the mapping worked" and "the pixels arrived" one
        // observation, and they fail independently.
        if (ev.type == WIN_EV_RAW_KEY && ev.a == 'f') {
            memset(&fb, 0, sizeof fb);
            fb.type = WIN_REQ_FB_MAP;
            if (sys_win_request(&fb) == 0) {
                fb_w = (unsigned)fb.a; fb_h = (unsigned)fb.b;
                fb_pitch = (unsigned)fb.c; fb_bpp = (unsigned)fb.d;
                have_fb = 1;
                volatile unsigned int *q = (volatile unsigned int *)WIN_FB_VADDR;
                unsigned probe = q[100u * (fb_pitch / 4) + 100u];
                snprintf(buf, sizeof buf, "compclient: fb %u %u %u %u probe %x\n",
                         fb_w, fb_h, fb_pitch, fb_bpp, probe);
            } else {
                snprintf(buf, sizeof buf, "compclient: fb REFUSED\n");
            }
            sys_eprint(buf);
        }

        if (ev.type == WIN_EV_RAW_KEY && ev.a == 'p' && have_fb && fb_bpp == 32) {
            // A 64x64 block of one known colour at a fixed offset, so a
            // test reads a pixel value rather than looking at a picture.
            // Written, never read back: the mapping is write-combining,
            // where a read is a full uncached round trip.
            volatile unsigned int *fbp = (volatile unsigned int *)WIN_FB_VADDR;
            unsigned stride_px = fb_pitch / 4;
            for (unsigned y = 0; y < 64 && y + 100 < fb_h; y++)
                for (unsigned x = 0; x < 64 && x + 100 < fb_w; x++)
                    fbp[(y + 100) * stride_px + (x + 100)] = 0x00FF00FFu;

            struct win_request_msg pr;
            memset(&pr, 0, sizeof pr);
            pr.type = WIN_REQ_FB_PRESENT;
            pr.a = 100; pr.b = 100; pr.c = 64; pr.d = 64;
            int ok = sys_win_request(&pr) == 0;
            unsigned back = fbp[132u * stride_px + 132u];
            snprintf(buf, sizeof buf, "compclient: painted %d readback %x\n", ok, back);
            sys_eprint(buf);
        }

        if (ev.type == WIN_EV_RAW_KEY && ev.a == 'r' && have_fb) {
            volatile unsigned int *fbp = (volatile unsigned int *)WIN_FB_VADDR;
            unsigned v = fbp[132u * (fb_pitch / 4) + 132u];
            snprintf(buf, sizeof buf, "compclient: reread %x\n", v);
            sys_eprint(buf);
        }

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
