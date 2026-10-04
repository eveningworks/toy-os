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
#include "irqflags.h" // irq_save(): every producer and the consumer share the queue
#include "kfmt.h"     // klog_printf -- notices past their reserve are said
#include "klog.h"     // KLOG_ERR
#include "ratelimit.h" // at most a line a second

// --- the queue --------------------------------------------------------
//
// ONE queue, for the one process that drains it, IN ARRIVAL ORDER: a
// SCREEN notice must not jump ahead of motion sampled under the old mode.
// Static rather than kmalloc'd because it is written from interrupt
// handlers, and allocating there would mean taking the heap's state in an
// interrupt.
//
// **A RELEASE IS NEVER SHED** -- a key's, a key position's, a button's --
// because a client that saw the press and not the release holds it
// forever. Such a slot is marked `keep`.
//
// **AND A NOTICE IS NEVER REFUSED.** The kernel's notices are FONT,
// SCREEN, SETTING and one FSWATCH per watch, each idempotent: a newer
// copy REMOVES the older and goes to the end, so at most
// WIN_INPUT_NOTICE_RESERVE are ever queued. Input -- polled or pushed --
// never holds more than the rest (WIN_INPUT_MAX): past that its oldest
// unmarked event goes, and with none the NEW input is refused. So a
// notice always finds room, and nothing marked is ever evicted.
//
// Pushed from process context (a setting, a watch, a mode change), from
// scheduler_idle() (win_input_poll()) and popped by the compositor's
// syscall -- all with interrupts off, which on one CPU serialises them.
static struct {
    struct win_event ring[WIN_EVENT_QUEUE_MAX];
    uint8_t keep[WIN_EVENT_QUEUE_MAX];   // by ring slot: a release, never evicted
    int head;    // next slot to pop
    int count;   // how many are queued
    int ninput;  // ...of which raw input
    int dropped; // overflow drops since the last reset
    int newest_edge; // the newest slot is a button EDGE: never merged into
} q;

void win_input_reset(void) {
    uint64_t f = irq_save();
    k_memset(&q, 0, sizeof q);
    irq_restore(f);
}

// Raw input. Everything else is a notice.
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

static int coalesces(const struct win_event *old, const struct win_event *ev) {
    if (old->type != ev->type) return 0;
    switch (ev->type) {
    case WIN_EV_SCREEN: case WIN_EV_FONT: case WIN_EV_SETTING: return 1;
    case WIN_EV_FSWATCH: return old->a == ev->a;
    default: return 0;
    }
}

// Removes the event `i` places from the head, closing the gap.
static void remove_at(int i) {
    if (is_input(q.ring[(q.head + i) % WIN_EVENT_QUEUE_MAX].type)) q.ninput--;
    for (int k = i; k < q.count - 1; k++) {
        int to = (q.head + k) % WIN_EVENT_QUEUE_MAX, from = (q.head + k + 1) % WIN_EVENT_QUEUE_MAX;
        q.ring[to] = q.ring[from];
        q.keep[to] = q.keep[from];
    }
    q.count--;
}

// `edge`: 0 for no button edge, EDGE_DOWN or EDGE_UP for one -- always
// the CALLER's truth (the poll knows its edges; a direct push says so
// through win_input_push_mouse_edge()), never guessed from the bits.
#define EDGE_DOWN WIN_INPUT_EDGE_DOWN
#define EDGE_UP   WIN_INPUT_EDGE_UP

// The one way an event is refused: counted, and a RELEASE said (at most
// a line a second, kernel/ratelimit.h) -- a client will hold that key or
// button. 0, for the caller to return.
static int refuse(const struct win_event *ev, int release) {
    static struct ratelimit rl;
    unsigned held = 0;
    q.dropped++;
    if (release && ratelimit_ok(&rl, &held))
        klog_printf(KLOG_ERR "win_input: a release (type %u) found no room "
                    "(%u more not logged)\n", ev->type, held);
    return 0;
}

// 1 queued (or merged), 0 refused. Interrupts are off.
static int enqueue(const struct win_event *ev, int edge) {
    int release = is_release(ev) || edge == EDGE_UP;
    if (is_input(ev->type)) {
        // MOTION IS A STATE, NOT A BACKLOG (Windows holds one WM_MOUSEMOVE
        // per queue; X compresses MotionNotify). A move with the same
        // buttons as the NEWEST queued move replaces it. Only the newest
        // slot, and never an EDGE: the drag that follows a press has the
        // press's mask, and merging it there moved the press to where the
        // drag ended.
        if (ev->type == WIN_EV_RAW_MOUSE && q.count > 0 && !edge && !q.newest_edge) {
            struct win_event *last = &q.ring[(q.head + q.count - 1) % WIN_EVENT_QUEUE_MAX];
            if (last->type == ev->type && last->mods == ev->mods) {
                *last = *ev;
                return 1;
            }
        }
        // Input at its share: the OLDEST unmarked input goes (the old end,
        // for kernel/win_input.h's reason); with none, the NEW one is
        // refused. Then never written over anything. win_input_poll()
        // takes from a source only with room, so neither refusal happens
        // to its events; only a direct push can be refused.
        if (q.ninput == WIN_INPUT_MAX) {
            int victim = -1;
            for (int i = 0; i < q.count && victim < 0; i++) {
                int slot = (q.head + i) % WIN_EVENT_QUEUE_MAX;
                if (is_input(q.ring[slot].type) && !q.keep[slot]) victim = i;
            }
            if (victim < 0) return refuse(ev, release);
            remove_at(victim);
            q.dropped++;
        }
        if (q.count == WIN_EVENT_QUEUE_MAX) return refuse(ev, release);
        q.ninput++;
    } else {
        for (int i = 0; i < q.count; i++)
            if (coalesces(&q.ring[(q.head + i) % WIN_EVENT_QUEUE_MAX], ev)) { remove_at(i); break; }
        // The reserve covers the kernel's four notice types; more notices
        // than that means a new kind that does not coalesce -- said, and
        // refused rather than written past the ring.
        if (q.count - q.ninput >= WIN_INPUT_NOTICE_RESERVE) {
            static struct ratelimit rl;
            unsigned held = 0;
            if (ratelimit_ok(&rl, &held))
                klog_printf(KLOG_ERR "win_input: %d notices queued, past the reserve of %d "
                            "(type %u; %u more not logged)\n", q.count - q.ninput + 1,
                            WIN_INPUT_NOTICE_RESERVE, ev->type, held);
        }
        if (q.count == WIN_EVENT_QUEUE_MAX) return refuse(ev, 0);
    }
    int slot = (q.head + q.count) % WIN_EVENT_QUEUE_MAX;
    q.ring[slot] = *ev;
    q.keep[slot] = (uint8_t)release;
    q.count++;
    q.newest_edge = edge != 0;
    return 1;
}

static int queue_push(const struct win_event *ev, int edge) {
    int pid = win_server_compositor_pid();
    if (!pid || !ev) return 0;
    uint64_t f = irq_save();
    int ok = enqueue(ev, edge);
    irq_restore(f);
    if (!ok) return 0;
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

int win_input_push_mouse_edge(const struct win_event *ev, int edge) {
    if (!ev || ev->type != WIN_EV_RAW_MOUSE) return 0;
    return queue_push(ev, edge == EDGE_UP ? EDGE_UP : EDGE_DOWN);
}

// What an input drain may take: input's share of the queue, unused.
// Taking only this, a poll never evicts.
static int input_room(void) { return WIN_INPUT_MAX - q.ninput; }

const void *win_input_wait_chan(void) { return &q; }

int win_input_pop(struct win_event *out) {
    if (!out) return 0;
    uint64_t f = irq_save();
    int got = q.count > 0;
    if (got) {
        *out = q.ring[q.head];
        if (is_input(out->type)) q.ninput--;
        q.keep[q.head] = 0;
        q.head = (q.head + 1) % WIN_EVENT_QUEUE_MAX;
        q.count--;
    }
    irq_restore(f);
    return got;
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

static int take_key(void) {
    int code = 0, down = 0;
    uint8_t mods = 0;
    if (!keyboard_try_get_key(&code, &down, &mods)) return 0;
    push(down ? WIN_EV_RAW_KEY : WIN_EV_RAW_KEY_UP, code, 0, mods);
    return 1;
}

static int take_phys(void) {
    uint16_t code = 0;
    int down = 0;
    uint8_t mods = 0;
    if (!keyboard_try_get_physical(&code, &down, &mods)) return 0;
    push(WIN_EV_RAW_KEY_PHYS, (int)code, down, mods);
    return 1;
}

// KEYS, in the order they happened (keyboard_try_get_key()), and THE SAME
// KEYS BY POSITION (WIN_EV_KEY_PHYS, always pushed: whether any window
// wants them is the compositor's to know), as far as input's share of
// the queue allows. EACH SLOT GOES TO THE STREAM NOT SERVED LAST, so with
// both backlogged they alternate however few slots a poll gets -- and a
// call that takes nothing changes nothing. Exported for its KTEST.
void win_input_drain_keys(void) {
    static int phys_next;   // the positional stream is owed the next slot
    while (input_room() > 0) {
        if (phys_next ? take_phys() : take_key()) phys_next = !phys_next;
        else if (!(phys_next ? take_key() : take_phys())) return;   // both empty
    }
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
    while (input_room() > 0 && mouse_try_get_button_edge(&edge, &ex, &ey)) {
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
    if (moved && input_room() > 0) {
        push(WIN_EV_RAW_MOUSE, x, y, g_last_buttons);
        moved = 0;
    }
    g_last_x = moved ? lx : x;   // unsent: still differs next poll
    g_last_y = moved ? ly : y;

    win_input_drain_keys();

    if (input_room() > 0) {           // the delta accumulates until read
        int wheel = mouse_get_wheel_delta();
        if (wheel != 0) push(WIN_EV_RAW_WHEEL, wheel, 0, 0);
    }
}
