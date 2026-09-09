// Raw input for a RING-3 compositor.
//
// THE GAP THIS CLOSES
// -------------------
// Stage 2 gave a registered compositor the raw input stream -- but the
// thing that PRODUCED it was the ring-0 WM: it polled the mouse and
// keyboard once per frame for its own routing, and forwarded a copy.
// That was the right shape while the WM was the desktop and a
// compositor was a second consumer.
//
// With the WM in ring 3 there is nobody left to poll. The desktop comes
// up, composites, and never receives a single event -- the pointer sits
// where it was seeded and no key or click ever arrives. That is exactly
// what the first person to run `gui3` reported: "totally stuck, even the
// cursor".
//
// So the kernel does it, which is where it always belonged: the devices
// are the kernel's, and a compositor is a process that cannot touch
// them.
//
// WHEN IT RUNS
// ------------
// Only when there is a compositor AND no registered ring-0 presentation
// layer. Both halves matter. While the ring-0 WM is up it polls the same
// devices for its own use, and a second reader would STEAL events from
// it -- `mouse_get_wheel_delta()` and `keyboard_try_getchar_mods()` both
// consume. So this stays silent for the whole of the migration and
// wakes up exactly when the WM leaves, which is the same condition
// win_server.c's compositor_gone() uses for the same reason.
//
// Called from scheduler_idle(), the kernel's one owner of idle work
// (R5) -- so it runs whoever is waiting, including `gui`'s
// spawn-and-wait loop, without any caller having to know about it.
#include "win_input.h"
#include "win_server.h"
#include "win_events.h"
#include "win_proto.h"
#include "mouse.h"
#include "gfx.h"    // gfx_width()/_height() -- the pointer's bounds
#include "keyboard.h"
#include "string.h"

// Last state pushed, so motion is reported on CHANGE rather than every
// poll. This runs far more often than a frame, and an unconditional push
// would overflow a 32-deep queue in a fraction of a second and report
// constant drops while the user sat still -- the same reasoning the
// ring-0 WM's own forwarder carried.
static int g_last_x = -1, g_last_y = -1;
static uint8_t g_last_buttons;

static void push(uint32_t type, int32_t a, int32_t b, uint32_t mods) {
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.a = a;
    ev.b = b;
    ev.mods = mods;
    win_events_push(win_server_compositor_pid(), &ev);
}

void win_input_poll(void) {
    int pid = win_server_compositor_pid();
    if (!pid) return;

    // THE DEVICE HAS TO BE STARTED, and this is the only place left that
    // can. The ring-0 WM called mouse_init()/mouse_set_bounds() itself;
    // a ring-3 compositor cannot touch hardware, and removing those
    // calls from it left nobody doing them -- so the pointer sat at
    // (0, 0) reporting nothing, which reads as a frozen desktop rather
    // than as an uninitialised device.
    //
    // Once, on the first poll after a compositor appears: bounds come
    // from the display, which is the same thing the WM used to pass.
    static int armed;
    if (!armed) {
        mouse_set_bounds(gfx_width(), gfx_height());
        mouse_init();
        armed = 1;
    }

    int x = 0, y = 0;
    uint8_t buttons = 0;
    mouse_get_state(&x, &y, &buttons);
    if (x != g_last_x || y != g_last_y || buttons != g_last_buttons) {
        // The hardware cursor plane rides HERE, not on a request: the
        // screen coordinates are already in hand, so pointer motion
        // costs the compositor zero syscalls and the plane moves even
        // before the WM's event loop wakes (win_proto.h's
        // WIN_REQ_FB_CURSOR arms this).
        if (win_server_hw_cursor_armed() && (x != g_last_x || y != g_last_y))
            gfx_hw_cursor_move(x, y);
        g_last_x = x;
        g_last_y = y;
        g_last_buttons = buttons;
        push(WIN_EV_RAW_MOUSE, x, y, buttons);
    }

    // Keys and wheel notches are CONSUMING reads, so each one is pushed
    // exactly once and there is no "on change" to apply -- a repeated
    // key is a real repeated key.
    uint8_t mods = 0;
    int key = keyboard_try_getchar_mods(&mods);
    if (key != -1) push(WIN_EV_RAW_KEY, key, 0, mods);

    // KEY TRANSITIONS: every release, and both edges of the four
    // modifier keys -- everything the byte stream above cannot carry
    // (api/keyboard.h). DRAINED, not sampled once: a press and its
    // release can both land inside one tick, and reporting only the
    // first would leave a client holding a key that is already up. The
    // budget bounds the loop against a device reporting nonsense, and is
    // the queue's own depth for the reason wm_rawin.c's is -- "how many
    // can be waiting?" has exactly that answer.
    uint16_t tcode = 0;
    int tdown = 0;
    uint8_t tmods = 0;
    for (int budget = 64; budget > 0; budget--) {
        if (!keyboard_try_get_transition(&tcode, &tdown, &tmods)) break;
        push(tdown ? WIN_EV_RAW_KEY : WIN_EV_RAW_KEY_UP, (int)tcode, 0, tmods);
    }

    int wheel = mouse_get_wheel_delta();
    if (wheel != 0) push(WIN_EV_RAW_WHEEL, wheel, 0, 0);
}
