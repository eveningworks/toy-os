// A ring-3 process that owns the SCREEN through ugfx's compositor
// surface -- Milestone 41 stage 4b's proof that the back buffer, the
// clip rect, the damage box and the publish path all work outside the
// kernel, before any of apps/wm/ moves.
//
// Separate from `compclient` on purpose. That one is stage 2's input
// path and owns no surface at all; this one draws and never asserts on
// input. Keeping them apart means a failure here cannot be a routing
// problem and a failure there cannot be a drawing one.
//
// WHAT IS AND IS NOT ASSERTABLE HERE, because getting this wrong cost
// real time on stage 4a:
//
//   The ring-0 WM is still compositing the same screen. So a block this
//   program presents survives only until the WM's next frame, and a test
//   that looks for it in a screenshot FAILS against a working kernel.
//   Two kinds of check work instead, and this program is built around
//   the split:
//
//   * **Back-buffer logic** -- clip, damage, blit offsets, the verify
//     diff. Entirely deterministic, entirely inside this process, and
//     these are the parts whose breakage would make a ring-3 WM draw
//     subtly wrong pixels forever. Asserted as numbers in the log.
//   * **The mapping is the real screen** -- proved by agreement between
//     what this program reads back through the mapping and what QEMU's
//     screendump reports for the same pixel, sampled at the same moment.
//     A mapping onto any other memory cannot produce agreement.
//
// Diagnostics go to stderr (sys_eprint) -- the kernel routes it into the
// kernel log and `dmesg`, where a test can read it. A windowed client's
// stdout goes nowhere useful and a spawned one's goes into its parent's
// pipe. See CLAUDE.md.
//
// Log grammar, one line per command, so a test asserts on text:
//   screenclient: registered
//   screenclient: screen <w> <h> <pitch> <bpp>
//   screenclient: screen REFUSED
//   screenclient: damage <x> <y> <w> <h>
//   screenclient: damage empty
//   screenclient: clip <drawn_outside> <dx> <dy> <dw> <dh>
//   screenclient: blit <p00> <p10> <p01> <p11>
//   screenclient: verify <count> <fx> <fy> <x0> <y0> <x1> <y1>
//   screenclient: probe <hex>
//   screenclient: present <x> <y> <w> <h> readback <hex>
//   screenclient: released
//   screenclient: exit
//
// Exits on 'q'. It owns no window, so there is no close button to click
// and wm_request_close() has nothing to ask.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/stdio.h"
#include "lib/string.h"
#include "ui/ugfx.h"
#include "win_proto.h"

// Colours picked to be distinguishable in a hex log at a glance and
// unlike anything the desktop theme draws, so a stray value in a
// readback is obviously not ours.
#define BASE_COLOR   0x00112233u
#define MARK_COLOR   0x00FF00FFu
#define SECOND_COLOR 0x0000FF00u

static int set_compositor(int claim) {
    struct win_request_msg req;
    memset(&req, 0, sizeof req);
    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = claim;
    return sys_win_request(&req);
}

static struct ugfx_screen g_sc;
static int g_have;

// Every check starts from a known screen: fill, then clear the damage,
// so the box a check reports covers only what that check drew. Without
// the reset each report would carry the whole screen from the fill and
// every damage assertion would read the same numbers.
static void reset_scene(void) {
    ugfx_clear_clip_rect(&g_sc.back);
    ugfx_fill(&g_sc.back, BASE_COLOR);
    ugfx_damage_reset(&g_sc.back);
}

// One scripted command. Split out of main()'s event loop so the same
// code can be driven WITHOUT keys -- see the `auto` mode below.
//
// Returns 0 to stop.
static int do_cmd(int cmd);

int main(int argc, char **argv) {
    char buf[128];

    if (set_compositor(1) != 1) {
        sys_eprint("screenclient: registration REFUSED\n");
        return 1;
    }
    sys_eprint("screenclient: registered\n");

    g_have = ugfx_screen_init(&g_sc);
    if (g_have) {
        snprintf(buf, sizeof buf, "screenclient: screen %d %d %u %d\n",
                 g_sc.back.w, g_sc.back.h, g_sc.pitch, g_sc.bpp);
    } else {
        // Says WHICH gate refused. A bare "REFUSED" is three different
        // failures -- the grant, the format, the heap -- wearing one
        // word, and telling them apart from outside is impossible.
        struct win_request_msg q;
        memset(&q, 0, sizeof q);
        q.type = WIN_REQ_FB_MAP;
        int rc = sys_win_request(&q);
        snprintf(buf, sizeof buf,
                 "screenclient: screen REFUSED rc %d geom %d %d %d %d\n",
                 rc, q.a, q.b, q.c, q.d);
    }
    sys_eprint(buf);

    // AUTO MODE: run the whole sequence with no keyboard at all.
    //
    // Raw keys reach a compositor only while nothing else is consuming
    // the keyboard, and when this client is the compositor there is no
    // desktop -- so the PHYSICAL SHELL has the keyboard and every
    // injected keystroke goes to it instead. That is not a bug in
    // either; it is what "the role is single" means once the window
    // manager is a process. A test that needs this client without a
    // desktop therefore cannot drive it with keys.
    //
    // Each step is announced first, so a reader (and the test tool) can
    // attribute a reply line to the command that produced it -- `d` and
    // `n` both log `damage`, and telling them apart by position in the
    // log is exactly the kind of fragility this avoids.
    if (argc > 1 && argv[1][0] == 'a') {
        // The probe runs twice: once early, and once at the very end so
        // a screendump taken right after the client exits is comparable
        // with it. Anything printed to the console in between repaints
        // the screen the probe just read.
        static const char SEQ[] = "rdcbvpnr";
        for (int i = 0; SEQ[i]; i++) {
            snprintf(buf, sizeof buf, "screenclient: step %c\n", SEQ[i]);
            sys_eprint(buf);
            if (g_have) do_cmd(SEQ[i]);
        }
        // HOLD the screen before releasing the role. The last probe
        // reports a pixel the test compares against a screendump, and
        // the moment this process exits the kernel restores the text
        // console and repaints over it -- so without this the two
        // observers read the screen at different times and the check
        // fails against a working mapping. Measured: the client
        // reported ff00ff and the screendump 000000.
        sys_eprint("screenclient: hold\n");
        unsigned long long until = sys_monotonic_ns() + 4000000000ULL;
        while (sys_monotonic_ns() < until) sys_yield();
    } else for (;;) {
        struct win_event ev;
        if (sys_wait_event(&ev) != 1) continue;
        if (ev.type != WIN_EV_RAW_KEY) continue;
        if (ev.a == 'q') break;
        if (!g_have) continue;
        do_cmd(ev.a);
    }

    if (set_compositor(0) == 1) sys_eprint("screenclient: released\n");
    sys_eprint("screenclient: exit\n");
    return 0;
}

static int do_cmd(int cmd) {
    char buf[128];
    int x, y, w, h;

    {
        switch (cmd) {

        // --- the damage box tracks exactly what was drawn -------------
        case 'd': {
            reset_scene();
            ugfx_fill_rect(&g_sc.back, 100, 100, 64, 64, MARK_COLOR);
            // A second, disjoint rect: the box is a BOUNDING box, so this
            // must widen it to cover both rather than replace it. A
            // per-rect implementation passes the single-rect case and
            // fails here, which is why both are in one check.
            ugfx_fill_rect(&g_sc.back, 300, 200, 10, 10, SECOND_COLOR);
            if (ugfx_damage(&g_sc.back, &x, &y, &w, &h))
                snprintf(buf, sizeof buf, "screenclient: damage %d %d %d %d\n", x, y, w, h);
            else
                snprintf(buf, sizeof buf, "screenclient: damage empty\n");
            sys_eprint(buf);
            break;
        }

        // --- the clip rect bounds both drawing and damage -------------
        case 'c': {
            reset_scene();
            ugfx_set_clip_rect(&g_sc.back, 200, 200, 10, 10);
            // Entirely outside the clip: must change nothing at all.
            ugfx_fill_rect(&g_sc.back, 300, 300, 50, 50, MARK_COLOR);
            int drawn_outside = (ugfx_get_pixel(&g_sc.back, 320, 320) != BASE_COLOR);
            // Straddling it: only the intersection may land, and the
            // damage must not exceed the clip either -- a clip honoured
            // for pixels but not for damage publishes a rect larger than
            // what changed, which is the harmless direction, and one
            // honoured for damage but not pixels is the bug that leaves
            // stale content on screen.
            ugfx_fill_rect(&g_sc.back, 195, 195, 20, 20, SECOND_COLOR);
            if (!ugfx_damage(&g_sc.back, &x, &y, &w, &h)) { x = y = w = h = -1; }
            ugfx_clear_clip_rect(&g_sc.back);
            snprintf(buf, sizeof buf, "screenclient: clip %d %d %d %d %d\n",
                     drawn_outside, x, y, w, h);
            sys_eprint(buf);
            break;
        }

        // --- a clipped blit reads from the right place ----------------
        case 'b': {
            reset_scene();
            // 4x4 of distinct values. The clip below keeps only the
            // bottom-right 2x2, so the pixels that land prove the blit
            // advanced its SOURCE pointer by the amount the clip took
            // off -- a blit that clips the destination and reads from the
            // source's origin draws the right count in the right box with
            // the wrong contents, and looks correct in a screenshot.
            uint32_t src[16];
            for (int i = 0; i < 16; i++) src[i] = 0x00100000u + (uint32_t)i;
            ugfx_set_clip_rect(&g_sc.back, 502, 502, 2, 2);
            ugfx_blit(&g_sc.back, 500, 500, 4, 4, src, 4);
            ugfx_clear_clip_rect(&g_sc.back);
            snprintf(buf, sizeof buf, "screenclient: blit %x %x %x %x\n",
                     ugfx_get_pixel(&g_sc.back, 502, 502),
                     ugfx_get_pixel(&g_sc.back, 503, 502),
                     ugfx_get_pixel(&g_sc.back, 502, 503),
                     ugfx_get_pixel(&g_sc.back, 503, 503));
            sys_eprint(buf);
            break;
        }

        // --- the verify diff finds exactly what changed ---------------
        case 'v': {
            reset_scene();
            if (!ugfx_verify_snapshot(&g_sc)) {
                sys_eprint("screenclient: verify unavailable\n");
                break;
            }
            // 3 wide by 2 tall = 6 pixels, at a known corner, so count,
            // first-difference and bounding box are all predictable to
            // the pixel rather than merely non-zero.
            ugfx_fill_rect(&g_sc.back, 400, 400, 3, 2, MARK_COLOR);
            struct ugfx_diff d;
            ugfx_verify_diff(&g_sc, &d);
            snprintf(buf, sizeof buf, "screenclient: verify %d %d %d %d %d %d %d\n",
                     d.count, d.first_x, d.first_y, d.x0, d.y0, d.x1, d.y1);
            sys_eprint(buf);
            ugfx_verify_release(&g_sc);
            break;
        }

        // --- the mapping is the real screen ---------------------------
        //
        // Reports a pixel read THROUGH the mapping without having
        // written anything, so a test can screendump the same point and
        // require agreement. Two independent observers of one address:
        // no other memory -- a zeroed frame, the wrong physical base,
        // another process's buffer -- can agree with the display here.
        //
        // Read-only and separate from 'p' on purpose. Doing it after a
        // write would race the ring-0 WM's next repaint, and the value
        // would then prove only that the write reached SOME writable
        // page. Reading the steady desktop races nothing.
        case 'r': {
            volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)WIN_FB_VADDR;
            uint32_t v = 0;
            if (g_sc.bpp == 32)
                v = fb[100u * (g_sc.pitch / 4u) + 100u] & 0x00FFFFFFu;
            snprintf(buf, sizeof buf, "screenclient: probe %x\n", v);
            sys_eprint(buf);
            break;
        }

        case 'p': {
            reset_scene();
            ugfx_fill_rect(&g_sc.back, 100, 100, 64, 64, MARK_COLOR);
            if (!ugfx_damage(&g_sc.back, &x, &y, &w, &h)) { x = y = w = h = -1; }
            ugfx_screen_present(&g_sc);

            // Reads the framebuffer directly, which the library itself
            // never does (it is write-combining -- see ugfx.h). Legitimate
            // here and only here: the whole point of the check is that
            // this address IS the display, and a test pairs this value
            // with a screendump of the same pixel taken at the same
            // moment. Compare the two; do not assert on either alone.
            volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)WIN_FB_VADDR;
            uint32_t back = 0;
            if (g_sc.bpp == 32)
                back = fb[132u * (g_sc.pitch / 4u) + 132u] & 0x00FFFFFFu;
            snprintf(buf, sizeof buf, "screenclient: present %d %d %d %d readback %x\n",
                     x, y, w, h, back);
            sys_eprint(buf);
            break;
        }

        // --- presenting nothing costs nothing -------------------------
        case 'n': {
            reset_scene();
            // Damage is empty, so this must publish no rect at all. The
            // check is that the NEXT present still reports the right box:
            // a present that cleared or corrupted an empty box would show
            // up as the following frame publishing the wrong region.
            ugfx_screen_present(&g_sc);
            ugfx_fill_rect(&g_sc.back, 50, 60, 8, 9, MARK_COLOR);
            if (!ugfx_damage(&g_sc.back, &x, &y, &w, &h)) { x = y = w = h = -1; }
            snprintf(buf, sizeof buf, "screenclient: damage %d %d %d %d\n", x, y, w, h);
            sys_eprint(buf);
            break;
        }

        default:
            break;
        }
    }
    return 1;
}
