// The compositor's input path. See win_input.h for what this is and
// win_role.c for who the compositor is.
//
// Reads here CONSUME (`mouse_get_wheel_delta()`,
// `keyboard_try_getchar_mods()`), so there must be exactly one reader:
// this poll, and only while a compositor holds the role.
#include "win_input.h"
#include "win_role.h"
#include "scheduler.h"   // scheduler_wake() -- the compositor parked on the queue
#include "futex.h"       // futex_note_ready() -- ...or on its wakeword
#include "mouse.h"
#include "gfx.h"    // gfx_width()/_height() -- the pointer's bounds
#include "keyboard.h"
#include "string.h"

// --- the queue --------------------------------------------------------
//
// ONE queue, for the one process that drains it. Static rather than
// kmalloc'd because it is written from interrupt handlers, and
// allocating there would mean taking the heap's state in an interrupt.
static struct {
    struct win_event ring[WIN_EVENT_QUEUE_MAX];
    int head;    // next slot to pop
    int count;   // how many are queued
    int dropped; // overflow drops since the last reset
} q;

void win_input_reset(void) {
    q.head = 0;
    q.count = 0;
    q.dropped = 0;
}

// Input is what a full queue sheds. Everything else -- WIN_EV_SCREEN,
// WIN_EV_FONT, a `gui` command -- is a fact the receiver cannot
// re-derive by looking.
static int is_input(uint32_t type) {
    switch (type) {
    case WIN_EV_RAW_MOUSE: case WIN_EV_RAW_KEY: case WIN_EV_RAW_KEY_UP: case WIN_EV_RAW_WHEEL:
        return 1;
    default:
        return 0;
    }
}

int win_input_push(const struct win_event *ev) {
    int pid = win_server_compositor_pid();
    if (!pid || !ev) return 0;

    // MOTION IS A STATE, NOT A BACKLOG (Windows holds one WM_MOUSEMOVE
    // per queue; X compresses MotionNotify). A move with the same
    // buttons as the NEWEST queued move replaces it, so motion never
    // holds more than one slot -- a mouse moving through one slow frame
    // evicted a WIN_EV_SCREEN and left the desktop painting the old
    // mode. Only the newest slot: a press in between keeps its place.
    if (ev->type == WIN_EV_RAW_MOUSE && q.count > 0) {
        struct win_event *last = &q.ring[(q.head + q.count - 1) % WIN_EVENT_QUEUE_MAX];
        if (last->type == ev->type && last->mods == ev->mods) {
            *last = *ev;
            scheduler_wake(&q, 0);
            futex_note_ready(pid);
            return 1;
        }
    }

    // PUSHED CONFIGURATION COALESCES: one pending per watch, one
    // SETTING. It says "look again", and a second copy says nothing the
    // first did not -- while a program writing all day would otherwise
    // fill the queue with them.
    if (ev->type == WIN_EV_FSWATCH || ev->type == WIN_EV_SETTING) {
        for (int i = 0; i < q.count; i++) {
            struct win_event *e = &q.ring[(q.head + i) % WIN_EVENT_QUEUE_MAX];
            if (e->type == ev->type && (ev->type == WIN_EV_SETTING || e->a == ev->a)) {
                *e = *ev;
                return 1;   // already queued; its wake is already pending
            }
        }
    }

    if (q.count == WIN_EVENT_QUEUE_MAX) {
        // Full: drop the OLDEST INPUT event, and only when there is
        // none the oldest of all. See WIN_EVENT_QUEUE_MAX's comment for
        // why the old end rather than refusing the new event.
        int victim = 0;
        for (int i = 0; i < q.count; i++)
            if (is_input(q.ring[(q.head + i) % WIN_EVENT_QUEUE_MAX].type)) { victim = i; break; }
        for (int i = victim; i < q.count - 1; i++)
            q.ring[(q.head + i) % WIN_EVENT_QUEUE_MAX] = q.ring[(q.head + i + 1) % WIN_EVENT_QUEUE_MAX];
        q.count--;
        q.dropped++;
    }

    q.ring[(q.head + q.count) % WIN_EVENT_QUEUE_MAX] = *ev;
    q.count++;

    // The woken syscall returns 0 ("try again"), NOT the event: this
    // may be running in an IRQ under some other process's CR3, so the
    // copy has to happen back inside the compositor's own syscall
    // (win_syscalls.c). The channel is the queue's address, so exactly
    // one process wakes -- and its wakeword beside it, for a receiver
    // waiting on its clients' rings as well (kernel/futex.h).
    scheduler_wake(&q, 0);
    futex_note_ready(pid);
    return 1;
}

const void *win_input_wait_chan(void) { return &q; }

int win_input_pop(struct win_event *out) {
    if (!out || q.count == 0) return 0;
    *out = q.ring[q.head];
    q.head = (q.head + 1) % WIN_EVENT_QUEUE_MAX;
    q.count--;
    return 1;
}

int win_input_pending(void) { return q.count; }
int win_input_dropped(void) { return q.dropped; }

// --- the devices ------------------------------------------------------

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
    win_input_push(&ev);
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

    // The hardware cursor plane rides HERE, not on a request: the
    // screen coordinates are already in hand, so pointer motion
    // costs the compositor zero syscalls and the plane moves even
    // before the WM's event loop wakes (win_proto.h's
    // WIN_REQ_FB_CURSOR arms this).
    if (win_server_hw_cursor_armed() && (x != g_last_x || y != g_last_y))
        gfx_hw_cursor_move(x, y);

    // BUTTON EDGES ARE DRAINED; THE POSITION IS SAMPLED. This poll runs
    // from scheduler_idle(), so on a busy machine a whole click can fall
    // between two passes -- comparing the mask against the last one seen
    // then reports nothing at all (api/mouse.h's transition queue). Each
    // edge carries the position it is being delivered at, which is why
    // draining one satisfies the motion push below as well.
    uint8_t edge = 0;
    int pushed = 0;
    for (int budget = 32; budget > 0; budget--) {
        if (!mouse_try_get_button_transition(&edge)) break;
        push(WIN_EV_RAW_MOUSE, x, y, edge);
        pushed = 1;
    }
    if (!pushed && (x != g_last_x || y != g_last_y))
        push(WIN_EV_RAW_MOUSE, x, y, buttons);
    g_last_x = x;
    g_last_y = y;
    g_last_buttons = buttons;

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
