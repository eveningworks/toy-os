// The ring-3 WM's input, received rather than polled (M41 stage 4c).
//
// THE INVERSION
// -------------
// The ring-0 WM asked the drivers for input once per frame:
// `mouse_get_state()` handed back a live position, `keyboard_try_
// getchar_mods()` pulled the next key. A ring-3 compositor cannot do
// that -- it does not own the hardware -- so the kernel DELIVERS input
// to it as `WIN_EV_RAW_MOUSE` / `_KEY` / `_WHEEL`, the events stage 2
// built and proved with `compclient`.
//
// The consequence that shapes this file: a poll answers "where is the
// pointer NOW", an event stream answers "what changed". So the position
// has to be kept HERE, updated as events arrive, and handed to the loop
// in the shape it already expects. That keeps wm.c's frame structure
// intact -- it still asks for a position at the top of the frame -- and
// confines the change to how that position is maintained.
//
// WHAT COLLAPSED WITH IT
// ----------------------
// Stage 2 deliberately ran TWO input paths at once: the WM polled the
// drivers, AND the kernel pushed the same input to a registered
// compositor, so the second consumer could be proved not to disturb the
// first (`compositor_test.py` asserts every injected event twice, in
// both logs). That duplication was always meant to end here -- once
// `WIN_EV_RAW_*` is the ONLY path, the WM's own `compositor_raw()`
// forwarder is forwarding to itself. It is deleted (R9).
//
// THE QUEUE IS FINITE, AND A DROP IS A REAL EVENT
// -----------------------------------------------
// The kernel's per-process event queue is 32 deep and drops the OLDEST
// when it overflows (see WIN_EV_RAW_MOUSE). So this drains everything
// available each frame rather than one event per frame: the WM's own
// frame rate is what bounds how fast it consumes, and taking one event
// per frame would guarantee a backlog under any real mouse movement.
//
// Motion is COALESCED on purpose -- ten queued moves are one position,
// and replaying each in turn would make the pointer crawl behind the
// user. Buttons, keys and wheel notches are NOT: each of those is a
// discrete thing the user did, and a control that arms on press and
// commits on release needs both halves to arrive.
#include "wm_internal.h"
#include "wm/wm_rawin.h"
#include "rt/sys.h"
#include "win_proto.h"

// The pointer, as the last WIN_EV_RAW_MOUSE left it. Seeded to the
// screen's centre the way mouse_init() used to, so the first frame has a
// sane position even if no motion has happened yet.
static int g_mx, g_my;
static uint8_t g_buttons;
static int g_seeded;

// Discrete events, held until the frame asks for them. One slot each
// rather than a queue: the loop consumes them every frame, and two keys
// arriving inside one frame is not something the hardware can produce at
// a 100Hz tick.
static int g_key = -1;
static uint8_t g_key_mods;
static int g_wheel;

void wm_rawin_init(int screen_w, int screen_h) {
    g_mx = screen_w / 2;
    g_my = screen_h / 2;
    g_buttons = 0;
    g_seeded = 1;
}

void wm_rawin_pump(void) {
    if (!g_seeded) return;

    struct win_event ev;
    // Non-blocking: the compositor must not park in the kernel waiting
    // for input, because it still owes the screen a frame, its clients
    // their timers, and the watchdog a measurement. sys_wait_event()
    // is what a CLIENT uses; a compositor polls and then does its work.
    while (sys_poll_event(&ev) == 1) {
        switch (ev.type) {
        case WIN_EV_RAW_MOUSE:
            // Coalesced by assignment -- the newest position wins.
            g_mx = ev.a;
            g_my = ev.b;
            g_buttons = (uint8_t)ev.mods;
            break;
        case WIN_EV_RAW_KEY:
            g_key = ev.a;
            g_key_mods = (uint8_t)ev.mods;
            break;
        case WIN_EV_RAW_WHEEL:
            // Accumulated, not replaced: two notches in one frame are
            // two notches of scrolling, and keeping only the last would
            // silently make a fast flick scroll less than a slow one.
            g_wheel += ev.a;
            break;
        default:
            // Anything else on this queue is a CLIENT request the kernel
            // is routing to the compositor. Not this file's business --
            // see wm_client.c. Deliberately not dropped silently once
            // that path exists; today nothing else arrives here.
            break;
        }
    }
}

void wm_rawin_mouse(int *out_x, int *out_y, uint8_t *out_buttons) {
    if (out_x) *out_x = g_mx;
    if (out_y) *out_y = g_my;
    if (out_buttons) *out_buttons = g_buttons;
}

int wm_rawin_take_key(uint8_t *out_mods) {
    int k = g_key;
    if (out_mods) *out_mods = g_key_mods;
    g_key = -1;
    return k;
}

uint8_t wm_rawin_mods_now(void) { return g_key_mods; }

int wm_rawin_take_wheel(void) {
    int w = g_wheel;
    g_wheel = 0;
    return w;
}
