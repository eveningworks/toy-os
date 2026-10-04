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
// THE DRAIN IS BOUNDED, AND THAT IS NOT AN OPTIMISATION
// ------------------------------------------------------
// The kernel's per-process event queue is 32 deep and drops the OLDEST
// when it overflows (see WIN_EV_RAW_MOUSE). One event per frame would
// guarantee a backlog under any real mouse movement -- but draining
// "until empty" is worse than either, and it LIVELOCKED the first
// desktop that ever had a client on it: a client presents every frame,
// each present is an event, and they arrived as fast as this consumed
// them, so the loop never exited, the frame never finished, and the
// desktop stopped answering the debug console while still showing its
// last good picture. A desktop that looks alive and is not.
//
// So: at most one queue's worth per frame. Bounded, which guarantees the
// frame completes, and generous, because the queue cannot hold more than
// that anyway -- anything still waiting is this frame's backlog and the
// next frame takes it.
//
// Motion is COALESCED on purpose -- ten queued moves are one position,
// and replaying each in turn would make the pointer crawl behind the
// user. Buttons, keys and wheel notches are NOT: each of those is a
// discrete thing the user did, and a control that arms on press and
// commits on release needs both halves to arrive. That last clause is
// literal for the keyboard now: a key event carries which edge it is.
#include "wm_internal.h"
#include "wm/wm_rawin.h"
#include "rt/sys.h"
#include "win_proto.h"
#include "keyboard.h"   // KEY_SHIFT / KEY_MOD_* -- the four modifier keys

// The pointer, as the last WIN_EV_RAW_MOUSE left it. Seeded to the
// screen's centre the way mouse_init() used to, so the first frame has a
// sane position even if no motion has happened yet.
static int g_mx, g_my;
static uint8_t g_buttons;
static int g_seeded;
// What this FRAME shows: an edge being replayed shows the position it
// happened at, else the newest position.
static int g_show_x, g_show_y;
static uint8_t g_show_mods;
static int g_show_edge;

// **BUTTON MASKS ARE A QUEUE, AND THE FRAME TAKES ONE PER FRAME.** The
// position above is a state and coalesces; a button change is a thing
// the user did. Assigning both from the newest event -- which is what
// this file did while the comment at the top claimed otherwise -- feeds
// wm.c's edge detector only the LAST mask of the frame, so a press and
// its release arriving together cancel and the click is gone. The
// kernel already keeps them apart (win_input.c pushes one event per
// edge, and its queue refuses to coalesce a move over a differing mask).
//
// AN EDGE KEEPS ITS POSITION AND MODIFIERS. Replayed a frame later, it
// must still say where it happened and what was held then -- a press at
// A and a release at B drained together used to replay as a press at B.
// Motion coalesces only up to the next edge (wm_rawin_mouse()).
// One more than a pump can deliver, since a full ring holds size - 1.
#define WM_RAWIN_BTNS 64
struct rawin_btn { uint8_t buttons, mods; int x, y; };
static struct rawin_btn g_btn_q[WM_RAWIN_BTNS];
static int g_btn_head, g_btn_tail;
// The newest mask SEEN, which is what a queued edge is compared against.
// g_buttons is the one being SHOWN this frame and can be several edges
// behind it; every reader in the WM must agree within a frame, so the
// queue advances in one place (the pump) rather than in the getter --
// five other files ask for the pointer and a consuming read there would
// let whichever ran first eat the frame's click.
static uint8_t g_btn_latest;

static uint8_t g_key_mods;

static void btn_push(uint8_t mask, int x, int y) {
    int next = (g_btn_head + 1) % WM_RAWIN_BTNS;
    // Oldest out, so the releases survive -- wm_rawin.c's key rule.
    if (next == g_btn_tail) g_btn_tail = (g_btn_tail + 1) % WM_RAWIN_BTNS;
    g_btn_q[g_btn_head] = (struct rawin_btn){ mask, g_key_mods, x, y };
    g_btn_head = next;
}

// Discrete events, held until the frame asks for them.
// One queue's worth. The same number as the kernel's depth rather than
// a tuned one, because "how many can be waiting?" has exactly that
// answer.
#define WM_RAWIN_DRAIN_MAX 32

// KEYS ARE A QUEUE -- see wm_rawin.h for why that changed when releases
// arrived. The frame takes one key, and wm.c does not park while any
// wait (wm_rawin_pending()), so a backlog drains at loop speed; the ring
// holds one full pump on top of one (a full ring holds size - 1).
#define WM_RAWIN_KEYS 64
struct rawin_key {
    int     code;
    uint8_t mods;
    uint8_t down;
};
static struct rawin_key g_keys[WM_RAWIN_KEYS];
static int g_key_head, g_key_tail;

// g_key_mods (above btn_push) is the modifiers as of the most recent key
// event, kept beside the queue rather than read out of it: "what is held
// now" without consuming anything.
static int g_wheel;

static void key_push(int code, uint8_t mods, int down) {
    int next = (g_key_head + 1) % WM_RAWIN_KEYS;
    // Dropping the OLDEST keeps the most recent releases, which are the
    // ones that un-stick a key. Unreachable in practice at this depth.
    if (next == g_key_tail) g_key_tail = (g_key_tail + 1) % WM_RAWIN_KEYS;
    g_keys[g_key_head].code = code;
    g_keys[g_key_head].mods = mods;
    g_keys[g_key_head].down = (uint8_t)(down ? 1 : 0);
    g_key_head = next;

    // WHAT IS HELD NOW, tracked from the MODIFIER KEYS' OWN transitions
    // rather than from the `mods` word riding on somebody else's key.
    // That word is sampled at scancode-processing time (api/keyboard.h)
    // and describes the key it came with; for a bare Shift held down
    // with nothing else pressed it is the only report there will be, and
    // trusting it left Shift+click reading as a plain click while
    // Ctrl+click worked. Left and right are one key here, as everywhere
    // else in this driver.
    uint8_t bit = 0;
    switch (code) {
    case KEY_SHIFT: bit = KEY_MOD_SHIFT; break;
    case KEY_CTRL:  bit = KEY_MOD_CTRL;  break;
    case KEY_ALT:   bit = KEY_MOD_ALT;   break;
    case KEY_ALTGR: bit = KEY_MOD_ALTGR; break;
    case KEY_SUPER: bit = KEY_MOD_SUPER; break;   // a modifier since shortcuts
    default: break;
    }
    if (bit) {
        if (down) g_key_mods |= bit;
        else      g_key_mods &= (uint8_t)~bit;
    } else {
        // An ordinary key: its own sampled word is authoritative, and
        // is what re-syncs this if a modifier's release was ever missed.
        g_key_mods = mods;
    }
}

void wm_rawin_init(int screen_w, int screen_h) {
    g_mx = screen_w / 2;
    g_my = screen_h / 2;
    g_buttons = 0;
    g_btn_latest = 0;
    g_btn_head = g_btn_tail = 0;
    g_show_x = g_mx;
    g_show_y = g_my;
    g_show_edge = 0;
    g_seeded = 1;
}

// After a mode change: a pointer past the new edge would be off-screen.
void wm_rawin_clamp(int screen_w, int screen_h) {
    if (g_mx >= screen_w) g_mx = screen_w - 1;
    if (g_my >= screen_h) g_my = screen_h - 1;
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_show_x >= screen_w) g_show_x = screen_w - 1;
    if (g_show_y >= screen_h) g_show_y = screen_h - 1;
}

void wm_rawin_pump(void) {
    if (!g_seeded) return;

    struct win_event ev;
    int budget = WM_RAWIN_DRAIN_MAX;
    // Non-blocking: the compositor must not park in the kernel waiting
    // for input, because it still owes the screen a frame, its clients
    // their timers, and the watchdog a measurement. sys_wait_event()
    // is what a CLIENT uses; a compositor polls and then does its work.
    while (budget-- > 0 && sys_poll_event(&ev) == 1) {
        switch (ev.type) {
        case WIN_EV_RAW_MOUSE:
            // Position coalesced by assignment -- the newest wins --
            // except that an edge also keeps its own.
            g_mx = ev.a;
            g_my = ev.b;
            if ((uint8_t)ev.mods != g_btn_latest) {
                g_btn_latest = (uint8_t)ev.mods;
                btn_push(g_btn_latest, ev.a, ev.b);
            }
            break;
        case WIN_EV_RAW_KEY:
            key_push(ev.a, (uint8_t)ev.mods, 1);
            break;
        case WIN_EV_RAW_KEY_UP:
            key_push(ev.a, (uint8_t)ev.mods, 0);
            break;
        case WIN_EV_RAW_KEY_PHYS:
            // Straight through: no shortcut, overlay or focus-ring step
            // applies to a key by position (wm_client.c).
            wm_client_route_phys_key(ev.a, ev.b, ev.mods);
            break;
        case WIN_EV_RAW_WHEEL:
            // Accumulated, not replaced: two notches in one frame are
            // two notches of scrolling, and keeping only the last would
            // silently make a fast flick scroll less than a slow one.
            g_wheel += ev.a;
            break;
        default:
            // Anything else on this queue is a CLIENT request the kernel
            // is routing to us -- a window created, presented, retitled.
            // Handed to wm_client.c, which turns it back into the same
            // call the ring-0 kernel used to make directly.
            //
            // An event neither this nor that recognises is DROPPED, and
            // that is deliberate: the protocol is allowed to grow, and a
            // compositor built against an older kernel should ignore
            // what it does not know rather than refuse to run.
            wm_client_handle_event(&ev);
            break;
        }
    }

    // ONE BUTTON EDGE PER FRAME, so wm.c's edge detector -- which
    // compares this frame's mask against last frame's -- sees every
    // press and every release. A tap whose two edges arrived in one
    // pump takes two frames to play out, which is what makes it a
    // click rather than nothing at all.
    if (g_btn_tail != g_btn_head) {
        const struct rawin_btn *e = &g_btn_q[g_btn_tail];
        g_buttons = e->buttons;
        g_show_x = e->x;
        g_show_y = e->y;
        g_show_mods = e->mods;
        g_show_edge = 1;
        g_btn_tail = (g_btn_tail + 1) % WM_RAWIN_BTNS;
    } else {
        g_show_x = g_mx;
        g_show_y = g_my;
        g_show_edge = 0;
    }
}

void wm_rawin_mouse(int *out_x, int *out_y, uint8_t *out_buttons) {
    if (out_x) *out_x = g_show_x;
    if (out_y) *out_y = g_show_y;
    if (out_buttons) *out_buttons = g_buttons;
}

uint8_t wm_rawin_pointer_mods(void) { return g_show_edge ? g_show_mods : g_key_mods; }

int wm_rawin_pending(void) {
    return g_key_tail != g_key_head || g_btn_tail != g_btn_head;
}

int wm_rawin_take_key(uint8_t *out_mods, int *out_down) {
    if (g_key_tail == g_key_head) {
        // The mods still answer, so a caller that asked for a key and
        // got none reads the same modifier state it would have read a
        // moment ago rather than zero.
        if (out_mods) *out_mods = g_key_mods;
        if (out_down) *out_down = 1;
        return -1;
    }
    struct rawin_key k = g_keys[g_key_tail];
    g_key_tail = (g_key_tail + 1) % WM_RAWIN_KEYS;
    if (out_mods) *out_mods = k.mods;
    if (out_down) *out_down = k.down;
    return k.code;
}

uint8_t wm_rawin_mods_now(void) { return g_key_mods; }

int wm_rawin_has_key(void) { return g_key_tail != g_key_head; }

int wm_rawin_take_wheel(void) {
    int w = g_wheel;
    g_wheel = 0;
    return w;
}
