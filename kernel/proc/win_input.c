// The compositor's input path. See win_input.h for what this is and
// win_role.c for who the compositor is.
//
// Reads here CONSUME (`mouse_get_wheel_delta()`,
// `keyboard_try_get_key()`), so there must be exactly one reader:
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
//
// **A RELEASE IS NEVER SHED** -- a key's, a key position's, a button's --
// because a client that saw the press and not the release holds it
// forever. Such a slot is marked `keep`; a full queue evicts only an
// unmarked input event and otherwise refuses the NEW event. Input from
// win_input_poll() never fills the last NOTICE_RESERVE slots, so a
// notice (WIN_EV_SCREEN, WIN_EV_FONT, a broadcast) always finds room
// over a backlog of releases; the idempotent ones coalesce, latest wins.
#define NOTICE_RESERVE 4
static struct {
    struct win_event ring[WIN_EVENT_QUEUE_MAX];
    uint8_t keep[WIN_EVENT_QUEUE_MAX];   // by ring slot: a release, never evicted
    int head;    // next slot to pop
    int count;   // how many are queued
    int dropped; // overflow drops since the last reset
    int newest_edge; // the newest slot is a button EDGE: never merged into
} q;

void win_input_reset(void) {
    q.head = 0;
    q.count = 0;
    k_memset(q.keep, 0, sizeof q.keep);
    q.dropped = 0;
    q.newest_edge = 0;
}

// Input is what a full queue sheds. Everything else -- WIN_EV_SCREEN,
// WIN_EV_FONT, a `gui` command -- is a fact the receiver cannot
// re-derive by looking.
static int is_input(uint32_t type) {
    switch (type) {
    case WIN_EV_RAW_MOUSE: case WIN_EV_RAW_KEY: case WIN_EV_RAW_KEY_UP: case WIN_EV_RAW_WHEEL:
    case WIN_EV_RAW_KEY_PHYS:
        return 1;
    default:
        return 0;
    }
}

// A key or key-position release (a button-up edge is told by its push).
static int is_release(const struct win_event *e) {
    return e->type == WIN_EV_RAW_KEY_UP || (e->type == WIN_EV_RAW_KEY_PHYS && !e->b);
}

// `edge`: 0 for no button edge, EDGE_DOWN or EDGE_UP for one.
#define EDGE_DOWN 1
#define EDGE_UP   2
static int queue_push(const struct win_event *ev, int edge) {
    int pid = win_server_compositor_pid();
    if (!pid || !ev) return 0;

    // MOTION IS A STATE, NOT A BACKLOG (Windows holds one WM_MOUSEMOVE
    // per queue; X compresses MotionNotify). A move with the same
    // buttons as the NEWEST queued move replaces it, so motion never
    // holds more than one slot -- a mouse moving through one slow frame
    // evicted a WIN_EV_SCREEN and left the desktop painting the old
    // mode. Only the newest slot, and never an EDGE: the drag that
    // follows a press has the press's mask, and merging it there moved
    // the press to where the drag ended.
    if (ev->type == WIN_EV_RAW_MOUSE && q.count > 0 && !edge && !q.newest_edge) {
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
    // A SCREEN or FONT notice is a state too: the newest says it all.
    if (ev->type == WIN_EV_FSWATCH || ev->type == WIN_EV_SETTING ||
        ev->type == WIN_EV_SCREEN || ev->type == WIN_EV_FONT) {
        for (int i = 0; i < q.count; i++) {
            struct win_event *e = &q.ring[(q.head + i) % WIN_EVENT_QUEUE_MAX];
            if (e->type == ev->type && (ev->type != WIN_EV_FSWATCH || e->a == ev->a)) {
                *e = *ev;
                return 1;   // already queued; its wake is already pending
            }
        }
    }

    if (q.count == WIN_EVENT_QUEUE_MAX) {
        // Full: drop the OLDEST INPUT event not marked `keep`. See
        // WIN_EVENT_QUEUE_MAX's comment for why the old end rather than
        // refusing the new event -- and with nothing evictable, the new
        // event IS refused, because a release is never the one shed.
        // win_input_poll() never fills the queue (queue_room()); this is
        // for a direct push.
        int victim = -1;
        for (int i = 0; i < q.count && victim < 0; i++) {
            int slot = (q.head + i) % WIN_EVENT_QUEUE_MAX;
            if (is_input(q.ring[slot].type) && !q.keep[slot]) victim = i;
        }
        if (victim < 0) {
            q.dropped++;
            return 0;
        }
        for (int i = victim; i < q.count - 1; i++) {
            int to = (q.head + i) % WIN_EVENT_QUEUE_MAX, from = (q.head + i + 1) % WIN_EVENT_QUEUE_MAX;
            q.ring[to] = q.ring[from];
            q.keep[to] = q.keep[from];
        }
        q.count--;
        q.dropped++;
    }

    int slot = (q.head + q.count) % WIN_EVENT_QUEUE_MAX;
    q.ring[slot] = *ev;
    q.keep[slot] = (uint8_t)(is_release(ev) || edge == EDGE_UP);
    q.count++;
    q.newest_edge = edge != 0;

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

int win_input_push(const struct win_event *ev) { return queue_push(ev, 0); }

// What an input drain may take: the free slots, less the notice reserve
// (see the queue above). Taking only this, a poll never evicts input.
static int queue_room(void) { return WIN_EVENT_QUEUE_MAX - NOTICE_RESERVE - q.count; }

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
static uint8_t g_last_buttons;   // the mask last PUSHED -- only an edge changes it

static void push_ev(uint32_t type, int32_t a, int32_t b, uint32_t mods, int edge) {
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.a = a;
    ev.b = b;
    ev.mods = mods;
    queue_push(&ev, edge);
}

static void push(uint32_t type, int32_t a, int32_t b, uint32_t mods) {
    push_ev(type, a, b, mods, 0);
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

    // BUTTON EDGES ARE DRAINED, EACH AT ITS OWN POSITION; THE POSITION
    // IS THEN SAMPLED. This poll runs from scheduler_idle(), so on a busy
    // machine a whole click can fall between two passes -- comparing the
    // mask against the last one seen then reports nothing at all
    // (api/mouse.h's transition queue). Motion after them carries the
    // last EDGE's mask, never the sampled level: an edge reported after
    // the drain would otherwise arrive as a mask change on a move, and
    // then again as an edge.
    int lx = g_last_x, ly = g_last_y;
    uint8_t edge = 0;
    // EVERY SOURCE BELOW IS READ ONLY WHILE THE QUEUE HAS ROOM, and what
    // does not fit stays in its own queue for the next poll -- so this
    // poll never makes queue_push() evict input to fit input.
    int ex = 0, ey = 0;
    while (queue_room() > 0 && mouse_try_get_button_edge(&edge, &ex, &ey)) {
        // An edge that lifts any button is a release: never shed.
        push_ev(WIN_EV_RAW_MOUSE, ex, ey, edge,
                (g_last_buttons & (uint8_t)~edge) ? EDGE_UP : EDGE_DOWN);
        g_last_buttons = edge;
        lx = ex;
        ly = ey;
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

    // Motion is a state: unsent for want of room, it goes next poll.
    int moved = (x != lx || y != ly);
    if (moved && queue_room() > 0) {
        push(WIN_EV_RAW_MOUSE, x, y, g_last_buttons);
        moved = 0;
    }
    g_last_x = moved ? lx : x;   // unsent: still differs next poll
    g_last_y = moved ? ly : y;

    // KEYS, in the order they happened (keyboard_try_get_key()), and THE
    // SAME KEYS BY POSITION (WIN_EV_KEY_PHYS, always pushed: whether any
    // window wants them is the compositor's to know). TAKEN IN TURN, and
    // which goes first alternates across polls too -- so even with one
    // free slot per poll a backlog in one cannot starve the other.
    static int phys_first;
    for (;;) {
        int took = 0;
        for (int k = 0; k < 2; k++) {
            if (queue_room() <= 0) break;
            if ((k == 0) == !phys_first) {
                int kcode = 0, kdown = 0;
                uint8_t kmods = 0;
                if (keyboard_try_get_key(&kcode, &kdown, &kmods)) {
                    push(kdown ? WIN_EV_RAW_KEY : WIN_EV_RAW_KEY_UP, kcode, 0, kmods);
                    took = 1;
                }
            } else {
                uint16_t tcode = 0;
                int tdown = 0;
                uint8_t tmods = 0;
                if (keyboard_try_get_physical(&tcode, &tdown, &tmods)) {
                    push(WIN_EV_RAW_KEY_PHYS, (int)tcode, tdown, tmods);
                    took = 1;
                }
            }
        }
        phys_first = !phys_first;
        if (!took) break;
    }

    if (queue_room() > 0) {           // the delta accumulates until read
        int wheel = mouse_get_wheel_delta();
        if (wheel != 0) push(WIN_EV_RAW_WHEEL, wheel, 0, 0);
    }
}
