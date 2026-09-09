// Raw input for the ring-3 compositor -- evdev's job, kept in the
// kernel for evdev's reason: the devices are the kernel's, and the
// compositor is a process that cannot touch them.
//
// Polled from scheduler_idle(), the kernel's one owner of idle work, so
// it runs whoever is waiting. Reads here CONSUME (`mouse_get_wheel_delta()`,
// `keyboard_try_getchar_mods()`), so there must be exactly one reader:
// this one, and only while a compositor holds the role.
#include "win_input.h"
#include "win_server.h"
#include "win_events.h"
#include "win_proto.h"
#include "mouse.h"
#include "gfx.h"    // gfx_width()/_height() -- the pointer's bounds
#include "keyboard.h"
#include "string.h"

// Last state pushed, so motion is reported on CHANGE rather than every
// poll: this runs far more often than a frame, and an unconditional push
// would overflow the queue in a fraction of a second while the user sat
// still.
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

    // THE DEVICE HAS TO BE STARTED, and this is the only place that can:
    // a ring-3 compositor cannot touch hardware, and a mouse nobody
    // initialised sits at (0, 0) reporting nothing, which reads as a
    // frozen desktop. Once, on the first poll after a compositor
    // appears; the bounds come from the display.
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
