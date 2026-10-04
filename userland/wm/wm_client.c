// Client windows, presentation side: the window manager acting as
// **TWS -- the Toy Window Server** -- for ring-3 processes. The
// protocol it serves is TWP (abi/win_proto.h); the client library on
// the other side is Toykit (userland/ui/).
//
// The split with kernel/proc/win_server.c is described in that file's
// header -- it owns the memory (ids, buffers, mappings, teardown),
// this owns the presentation (a slot in windows[], chrome, geometry,
// z-order, input routing). Everything crossing between them is a typed
// message from abi/win_proto.h, which is what makes the server movable
// to ring 3 later (docs/roadmap.md's Milestone 41).
//
// Why this is a separate file rather than more of wm.c: it's a distinct
// concern with its own external contract, exactly the split that
// produced desktop.c/start_menu.c/context_menu.c. See wm.c's top
// comment.
#include "wm/wm_watch.h" // WIN_EV_FSWATCH / WIN_EV_SETTING land there
#include <stdlib.h>   // malloc: the client widget map
#include "wm_internal.h"
#include "crash_notice.h"
#include "wm_peek.h"
#include "wm_shadow.h"
#include "wm_anim.h"   // wm_damage_window_rect(): a window's rect plus its shadow
#include "wm_shortcut.h"
#include "lib/usaver.h"
#include "wm_idle.h"
#include "wm_dnd.h"
#include "diag_abi.h"
#include "lib/uchan.h"
#include "lib/uwmchan.h"
#include "wm_geometry.h"
#include "wm_overlay.h" // WM_POPUP_MARGIN -- a client popup keeps the same edge gap
#include "wm_rawin.h"
#include "wm_debug.h" // the diagnostic channel's WM end, below
#include "wm_screenshot.h"
#include "win_role.h"
#include "kapi.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h" // uui_hit() -- the popup route walk
#include "rt/sys.h"
#include "wm/wm_log.h"

// --- delivering events to clients ------------------------------------
//
// Into the client's INBOX on its own channel ring (lib/uwmchan.h) --
// this process writes, the client drains, and the kernel carries
// nothing. Returns 1 on success, 0 when the inbox is full or the client
// has no ring. A full inbox is the client not keeping up, which the
// compositor cannot fix; what it decides is whether the event is worth
// keeping until there is room (send_state below) or not (input).
static struct uchan_server g_chan;

static int win_events_push(int pid, const struct win_event *ev) {
    if (!ev) return 0;
    return uchan_server_send(&g_chan, pid, ev, sizeof *ev) == 0;
}

// Input to a client that could not take it. Counted on the window so
// `gui windows` can name the client that is behind, and logged once
// per window rather than per event.
static void note_dropped(struct window *win) {
    if (!win->ev_dropped++)
        wm_logf("wm: client pid %d is not draining its events\n", win->client_pid);
}

// A STATE event: sent now if there is room, otherwise remembered and
// sent by wm_client_flush_pending() with the state as it is THEN. The
// caller fills the payload fields on the window before calling.
enum {
    WM_PEND_CLOSE  = 1 << 0,
    WM_PEND_RESIZE = 1 << 1,
    WM_PEND_FOCUS  = 1 << 2,
    WM_PEND_FONT   = 1 << 3,
    WM_PEND_SCREEN = 1 << 4,
    WM_PEND_SETTING = 1 << 5,
};

static int send_state(struct window *win, const struct win_event *ev, uint32_t bit) {
    if (win_events_push(win->client_pid, ev)) {
        win->ev_pending &= ~bit;
        return 1;
    }
    win->ev_pending |= bit;
    return 0;
}

static void send_popup_done(struct window *toplevel, uint32_t slot) {
    struct win_event ev = {0};
    ev.type = WIN_EV_POPUP_DONE;
    ev.window = slot;
    if (win_events_push(toplevel->client_pid, &ev)) toplevel->ev_popup_done &= ~(1u << slot);
    else toplevel->ev_popup_done |= 1u << slot;
}

static int find_client_window(int pid, uint32_t id);
static void send_release(struct window *win, int b);

void wm_client_flush_pending(void) {
    for (int i = 0; i < window_count; i++) {
        struct window *win = &windows[i];
        if (!wm_client_is_client_window(win)) continue;
        if (!win->ev_pending && !win->ev_popup_done && !win->ev_release) continue;
        struct win_event ev = {0};
        ev.window = win->client_win;
        if (win->ev_pending & WM_PEND_CLOSE) {
            ev.type = WIN_EV_CLOSE;
            if (!send_state(win, &ev, WM_PEND_CLOSE)) continue;
        }
        if (win->ev_pending & WM_PEND_RESIZE) {
            ev.type = WIN_EV_RESIZE; ev.a = win->ev_resize_w; ev.b = win->ev_resize_h;
            if (!send_state(win, &ev, WM_PEND_RESIZE)) continue;
        }
        if (win->ev_pending & WM_PEND_FOCUS) {
            ev.type = WIN_EV_FOCUS; ev.a = (wm_focus_index() == i) ? 1 : 0; ev.b = 0;
            if (!send_state(win, &ev, WM_PEND_FOCUS)) continue;
        }
        if (win->ev_pending & WM_PEND_SCREEN) {
            ev.type = WIN_EV_SCREEN; ev.a = screen_w; ev.b = screen_h;
            if (!send_state(win, &ev, WM_PEND_SCREEN)) continue;
        }
        if (win->ev_pending & WM_PEND_FONT) {
            ev.type = WIN_EV_FONT; ev.a = 0; ev.b = 0;
            if (!send_state(win, &ev, WM_PEND_FONT)) continue;
        }
        if (win->ev_pending & WM_PEND_SETTING) {
            ev.type = WIN_EV_SETTING; ev.a = 0; ev.b = 0;
            if (!send_state(win, &ev, WM_PEND_SETTING)) continue;
        }
        for (uint32_t slot = 1; slot < 32 && win->ev_popup_done; slot++)
            if (win->ev_popup_done & (1u << slot)) send_popup_done(win, slot);
        for (int b = 0; b < WIN_CLIENT_BUFS && win->ev_release; b++)
            if (win->ev_release & (1u << b)) send_release(win, b);
    }
}

// The session changed under every client -- the font, or the screen.
// One event per PROCESS: its popups are surfaces of the same app, and
// the toplevel's row is the one that outlives them.
static void broadcast_state(uint32_t type, int32_t a, int32_t b, uint32_t bit) {
    for (int i = 0; i < window_count; i++) {
        struct window *win = &windows[i];
        if (!wm_client_is_client_window(win) || win->popup) continue;
        struct win_event ev = {0};
        ev.type = type;
        ev.window = win->client_win;
        ev.a = a;
        ev.b = b;
        send_state(win, &ev, bit);
    }
}

// Finds the windows[] index for one client window, or -1. Linear, and
// it avoids caching an index that a close or a raise renumbers.
static int find_client_window(int pid, uint32_t id) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].client_pid == pid && windows[i].client_win == id) return i;
    }
    return -1;
}

// wm.h mirrors the ABI's id length rather than including it. If those
// two ever disagree the failure is silent and nasty: ids truncate to the
// shorter one, so two apps whose names share a prefix start matching
// each other and raising each other's windows.
_Static_assert(WIN_APP_ID_MAX == WIN_APP_ID_LEN,
                "wm.h's WIN_APP_ID_MAX must match abi/win_proto.h's WIN_APP_ID_LEN");

static int map_buf(struct window *win, int b, uint32_t gen, int w, int h);
static void unmap_client_window(struct window *win);

// The frame around a content size: chrome for a toplevel, none for a
// popup. The two adopt sites (create, present) must agree with
// window_content_*() in wm.c, and this is how they do.
static void adopt_content_size(struct window *win, int w, int h) {
    win->client_w = w;
    win->client_h = h;
    win->w = window_has_chrome(win) ? w + 2 : w;
    win->h = window_has_chrome(win) ? h + WM_TITLEBAR_H + 2 : h;
}

static int on_window_created(int pid, uint32_t id,
                              int w, int h, int x, int y,
                              const char *app_id, int app_identity) {
    // Grow the table instead of refusing at a fixed count. A refusal is
    // still a legal protocol outcome (the client sees the create fail),
    // but now it means the kernel is out of memory rather than that the
    // desktop already has six windows.
    if (!wm_windows_reserve(window_count + 1)) return 0;

    // Same "the window losing focus must repaint its title bar" damage
    // open_app() does, and for the same reason -- a new window steals
    // focus, and the old frontmost one's blue title bar would otherwise
    // stay blue until something unrelated repainted it. That exact bug
    // is documented in open_app().
    if (window_count > 0) {
        struct window *losing_focus = &windows[window_count - 1];
        wm_damage_window_rect(losing_focus->x, losing_focus->y,
                               losing_focus->w, losing_focus->h);
    }

    struct window *win = &windows[window_count];
    k_memset(win, 0, sizeof(*win));

    // A client asking for (0,0) gets the same cascade an app window
    // would, rather than every client stacking exactly on the origin.
    int cascade = (window_count % 5) * 24;
    win->x = (x > 0) ? x : 60 + cascade;
    win->y = (y > 0) ? y : 40 + cascade;
    adopt_content_size(win, w, h);
    win->state = WIN_NORMAL;
    win->app = 0;        // a client window has no gui_app -- see wm.h
    win->open = 1;
    win->client_pid = pid;
    win->client_win = id;
    win->client_front = 0;    // until the first present
    // BUFFER 0 AT GENERATION 0: a window's first object under each of
    // its two names. The other one is opened when a present first
    // points at it, which is also how every later object is picked up.
    if (!map_buf(win, 0, 0, w, h)) {
        wm_logf("wm: client pid %d window %u -- cannot open its buffer\n", pid, id);
        return 0;
    }
    win->client_buf = win->client_px[0];
    win->client_last_mx = INT32_MIN; // nothing delivered yet
    win->client_last_my = INT32_MIN;
    win->ev_pending = 0;
    win->ev_popup_done = 0;
    win->ev_dropped = 0;

    // The IDENTITY the taskbar groups by -- see wm.h. -1 means "no
    // identity", which groups with nothing.
    win->app_identity = app_identity;

    // Already truncated by win_server.c, and "" when the client named
    // nothing. A DISPLAY name only.
    if (app_id) {
        int i = 0;
        for (; app_id[i] && i < WIN_APP_ID_MAX - 1; i++) win->app_id[i] = app_id[i];
        win->app_id[i] = '\0';
    }

    // A placeholder until the client sends WIN_REQ_TITLE. Deliberately
    // not left blank: an untitled window is indistinguishable from a
    // broken one on screen.
    const char *deflt = "Client";
    int i = 0;
    for (; deflt[i] && i < WIN_TITLE_MAX - 1; i++) win->title[i] = deflt[i];
    win->title[i] = '\0';

    // A new window steals focus; wm_focus_sync() tells both clients.

    windows[window_count].open_seq = wm_next_open_seq();
    window_count++;
    // AFTER window_count++ (restoring addresses windows by index) and
    // after app_id is set, which is what the saved geometry is keyed
    // on. A client's SIZE cannot simply be assigned -- it owns its
    // buffer -- so this may send it a resize to ask for one; see
    // wm_geometry.c.
    wm_geometry_restore(window_count - 1);
    redraw_pending = 1;
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h); // new taskbar button

    wm_logf("wm: client pid %d opened window %u (%dx%d)\n", pid, id, w, h);
    return 1;
}

// --- popup surfaces (abi/win_proto.h's WIN_REQ_POPUP) -----------------
//
// The positioner, resolved here rather than in the client because only
// this process knows where the parent is and where the taskbar is. Flip
// to the other side of the anchor when the preferred side does not fit,
// slide along the other axis, clamp last -- the arithmetic
// ui/uui_menubar.c resolves against a window, against the WORK AREA. The
// right and bottom edges keep WM_POPUP_MARGIN like every WM popup
// (wm_popup_place()); the left and top do not, so a menu dropping from a
// title flush against the screen edge is not nudged off its title.
static void place_popup(const struct window *parent, const struct win_popup_pos *p,
                        int w, int h, int *out_x, int *out_y) {
    int ax = window_content_x(parent) + p->ax;
    int ay = window_content_y(parent) + p->ay;
    int aw = p->aw, ah = p->ah;
    int x1 = screen_w - WM_POPUP_MARGIN;
    int y1 = screen_h - taskbar_h - WM_POPUP_MARGIN;
    int x, y;
    if (p->gravity == WIN_POPUP_RIGHT) {
        x = ax + aw;
        y = ay;
        if (x + w > x1 && ax - w >= 0) x = ax - w;   // flip left
    } else {
        x = ax;
        y = ay + ah;
        if (y + h > y1 && ay - h >= 0) y = ay - h;   // flip above
    }
    if (x + w > x1) x = x1 - w;                      // slide
    if (x < 0) x = 0;                                // clamp
    if (y + h > y1) y = y1 - h;
    if (y < 0) y = 0;
    *out_x = x;
    *out_y = y;
}

// A popup joins windows[] AT THE TOP, as an ordinary client window with
// `popup` set: it inherits the blit, the damage tracking, the ping and
// the dead-client sweep for nothing, and the four accessors in wm.c make
// it chromeless. Nothing is told about focus -- the parent keeps it, see
// wm_focus_index() -- and no geometry is restored or saved.
static int on_popup_created(int pid, uint32_t id, uint32_t parent_id, int w, int h,
                            const struct win_popup_pos *pos, int *out_x, int *out_y) {
    if (w <= 0 || h <= 0) return 0;
    if (find_client_window(pid, parent_id) < 0) return 0;
    if (!wm_windows_reserve(window_count + 1)) return 0;
    // AFTER the reserve, which may move the array.
    const struct window *parent = &windows[find_client_window(pid, parent_id)];

    int sx, sy;
    place_popup(parent, pos, w, h, &sx, &sy);
    *out_x = sx - window_content_x(parent);
    *out_y = sy - window_content_y(parent);

    struct window *win = &windows[window_count];
    k_memset(win, 0, sizeof(*win));
    win->popup = 1;
    win->popup_grab = (pos->flags & WIN_POPUP_GRAB) != 0;
    win->popup_glass = (pos->flags & WIN_POPUP_GLASS) != 0;
    win->popup_parent = parent_id;
    win->x = sx;
    win->y = sy;
    adopt_content_size(win, w, h);
    win->state = WIN_NORMAL;
    win->open = 1;
    win->client_pid = pid;
    win->client_win = id;
    win->app_identity = parent->app_identity;
    if (!map_buf(win, 0, 0, w, h)) {
        wm_logf("wm: client pid %d popup %u -- cannot open its buffer\n", pid, id);
        return 0;
    }
    win->client_buf = win->client_px[0];
    win->client_last_mx = INT32_MIN;
    win->client_last_my = INT32_MIN;
    k_strlcpy(win->title, "Popup", sizeof win->title);

    windows[window_count].open_seq = wm_next_open_seq();
    window_count++;
    redraw_pending = 1;  // compute_window_damage() sees a new rect
    return 1;
}

// --- dialog windows (abi/win_proto.h's WIN_REQ_DIALOG) ---------------
//
// A second TOPLEVEL of the same client, so it joins windows[] as an
// ordinary window and keeps its chrome, its focus and its Alt+F4 -- the
// opposite of a popup, which is the same list entry with all three
// taken away. What it does NOT get: a taskbar button (wm_taskbar.c's
// unlisted(), as Win32 skips an owned window), saved geometry (a dialog
// opens where its owner is, not where the last one was), and a place in
// the stack below its owner (raise_with_dialogs(), wm.c).
static int on_dialog_created(int pid, uint32_t id, uint32_t owner_id,
                             int w, int h, int flags, const char *title) {
    if (w <= 0 || h <= 0) return 0;
    int oi = find_client_window(pid, owner_id);
    // A DIALOG OF A DIALOG IS REFUSED rather than flattened: the owner
    // chain is what `modal` is resolved against, and one level is all
    // the client side can express anyway.
    if (oi < 0 || windows[oi].popup || windows[oi].dialog) return 0;
    if (!wm_windows_reserve(window_count + 1)) return 0;
    const struct window *owner = &windows[find_client_window(pid, owner_id)];

    struct window *win = &windows[window_count];
    k_memset(win, 0, sizeof(*win));
    win->dialog = 1;
    win->dialog_owner = owner_id;
    win->modal = (flags & WIN_DIALOG_MODAL) != 0;
    adopt_content_size(win, w, h);
    // CENTRED ON THE OWNER, unless it does not FIT over the owner -- a
    // chooser is usually wider than the little window that asked for it,
    // and centring on a narrower owner then clamps it hard against a
    // screen edge. Centre it on the work area instead, which is what it
    // would have looked like anyway.
    int wa_h = screen_h - taskbar_h;
    win->x = win->w <= window_content_w(owner)
                 ? window_content_x(owner) + (window_content_w(owner) - win->w) / 2
                 : (screen_w - win->w) / 2;
    win->y = win->h <= window_content_h(owner)
                 ? window_content_y(owner) + (window_content_h(owner) - win->h) / 2
                 : (wa_h - win->h) / 2;
    if (win->x + win->w > screen_w) win->x = screen_w - win->w;
    if (win->y + win->h > wa_h) win->y = wa_h - win->h;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;
    win->state = WIN_NORMAL;
    win->open = 1;
    win->client_pid = pid;
    win->client_win = id;
    win->app_identity = owner->app_identity;
    if (!map_buf(win, 0, 0, w, h)) {
        wm_logf("wm: client pid %d dialog %u -- cannot open its buffer\n", pid, id);
        return 0;
    }
    win->client_buf = win->client_px[0];
    win->client_last_mx = INT32_MIN;
    win->client_last_my = INT32_MIN;
    k_strlcpy(win->title, (title && title[0]) ? title : "Dialog", sizeof win->title);

    // The owner is losing focus to it (wm_focus_sync() says so), and its
    // title bar shows it.
    wm_damage_window_rect(windows[window_count - 1].x, windows[window_count - 1].y,
                          windows[window_count - 1].w, windows[window_count - 1].h);

    windows[window_count].open_seq = wm_next_open_seq();
    window_count++;
    redraw_pending = 1;
    wm_logf("wm: client pid %d opened dialog %u (%dx%d) of %u\n", pid, id, w, h, owner_id);
    return 1;
}

int wm_dialog_blocker(int idx) {
    if (idx < 0 || idx >= window_count) return -1;
    const struct window *o = &windows[idx];
    if (o->dialog || o->popup || !o->client_pid) return -1;
    for (int i = window_count - 1; i >= 0; i--)
        if (windows[i].dialog && windows[i].modal &&
            windows[i].client_pid == o->client_pid &&
            windows[i].dialog_owner == o->client_win) return i;
    return -1;
}

// Every dialog of this owner that is currently BELOW it, promoted in
// turn. Each promotion renumbers windows[], so the scan restarts rather
// than holding an index across one; a dialog already above its owner is
// left alone, which is what ends the loop.
void wm_dialogs_raise(int pid, uint32_t owner_win) {
    if (!pid) return;
    for (;;) {
        int oi = find_client_window(pid, owner_win);
        if (oi < 0) return;
        int d = -1;
        for (int i = 0; i < oi; i++)
            if (windows[i].dialog && windows[i].client_pid == pid &&
                windows[i].dialog_owner == owner_win) { d = i; break; }
        if (d < 0) return;
        bring_to_front(d);
    }
}

int wm_dialog_of(int idx, int after) {
    if (idx < 0 || idx >= window_count) return -1;
    const struct window *o = &windows[idx];
    if (o->dialog || !o->client_pid) return -1;
    for (int i = after + 1; i < window_count; i++)
        if (windows[i].dialog && windows[i].client_pid == o->client_pid &&
            windows[i].dialog_owner == o->client_win) return i;
    return -1;
}

// ONLY A GRABBING POPUP OWNS THE POINTER. A tooltip is a popup with no
// grab, and answering with its pid would make every press anywhere
// dismiss it and be consumed -- which is a tooltip swallowing the click
// that was meant for a button.
int wm_client_popup_owner(void) {
    for (int i = window_count - 1; i >= 0; i--)
        if (windows[i].popup && windows[i].popup_grab) return windows[i].client_pid;
    return 0;
}

// The same topmost-first walk wm_handle_left_click() does, so the two
// agree on which window a press is "in".
int wm_client_popup_route(int owner, int mx, int my) {
    for (int i = window_count - 1; i >= 0; i--) {
        const struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (w->client_pid != owner) return 0;
        if (w->popup) return 1;
        return uui_hit(window_content_x(w), window_content_y(w),
                       window_content_w(w), window_content_h(w), mx, my) ? 2 : 0;
    }
    return 0;
}

// Downward, because close_window() shifts everything above the slot.
void wm_client_popups_dismiss(int owner) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (i >= window_count) continue;
        struct window *w = &windows[i];
        if (!w->popup || w->client_pid != owner) continue;
        // Through the TOPLEVEL's bookkeeping: this row is about to go,
        // and a dismissal the inbox had no room for must still arrive.
        int top = find_client_window(owner, 0);
        if (top >= 0) send_popup_done(&windows[top], w->client_win);
        unmap_client_window(w);
        w->client_pid = 0;
        close_window(i);
    }
    redraw_pending = 1;
}

// `front` is which of the window's two buffers now holds finished
// pixels -- the kernel flipped it inside WIN_REQ_PRESENT, before this
// event was queued, so by the time this runs the client is already
// drawing into the other one.
static void send_release(struct window *win, int b) {
    struct win_event ev = {0};
    ev.type = WIN_EV_BUF_RELEASE;
    ev.window = win->client_win;
    ev.a = b;
    ev.b = (int32_t)win->client_gen[b];
    // OWED, NOT DROPPED: the client holds this buffer until told, and a
    // lost release costs it a frame-time guess (uapp's forced reuse).
    if (win_events_push(win->client_pid, &ev)) win->ev_release &= ~(1u << b);
    else win->ev_release |= 1u << b;
}

static void on_window_present(int pid, uint32_t id, int front, uint32_t gen,
                              int w, int h) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    wm_scanout_client_presented(pid);   // a present from its OWN buffer

    struct window *win = &windows[idx];
    // THE FIRST FRAME is when the window appears, and so when it
    // animates in (wm_anim.h) -- after this present is adopted, since
    // the ghost is a snapshot of what arrived.
    int first = 1;
    for (int b = 0; b < WIN_CLIENT_BUFS; b++)
        if (win->client_gen[b]) first = 0;
    if (front < 0 || front >= WIN_CLIENT_BUFS) return;

    // **THE GENERATION IS WHAT SAYS "RE-OPEN THE NAME".** The name is
    // the slot and never changes; the object under it does, on every
    // resize. Getting a stale one costs a frame; not noticing at all
    // would be this process blitting an object nobody is drawing into.
    // A failed open leaves the last good frame on screen -- see
    // map_buf() on why the size is checked by mapping it.
    if (!map_buf(win, front, gen, w, h)) return;
    int old = win->client_front;
    win->client_front = front;
    win->client_buf = win->client_px[front];
    win->ev_release &= ~(1u << front);   // on screen again: not free after all
    // THE OLD FRONT IS FREE NOW, and only now: nothing reads it after
    // this, and no composite is in flight while a message is handled.
    // Without this the client redrew it while a preempted composite was
    // still reading it -- a half-painted window on screen.
    if (old != front) send_release(win, old);
    wm_peek_presented(win->open_seq);   // its peek thumbnail is stale
    // THE FRAME BRINGS ITS OWN SIZE, and this is where a resize lands.
    // Adopting it when the client ACCEPTED the proposal instead would
    // put the chrome around a buffer with nothing in it yet -- a whole
    // window of black for the length of the client's repaint. See
    // abi/win_proto.h's configure/ack.
    if (w > 0 && h > 0 && (w != win->client_w || h != win->client_h)) {
        wm_damage_window_rect(win->x, win->y, win->w, win->h);   // the rect being left
        adopt_content_size(win, w, h);
        wm_damage_window_rect(win->x, win->y, win->w, win->h);
        if (first) wm_anim_open(idx);
        redraw_pending = 1;
        // An interactive resize sends its next proposal now: one FRAME
        // in flight at a time, so the client's real repaint sets the
        // pace rather than its syscall latency.
        wm_resize_shown(idx, 1);
        return;
    }

    // Damage only the CONTENT area, not the whole window: the chrome
    // hasn't changed, and over-damaging is how a compositor quietly
    // stops being a compositor. EXCEPT THE FIRST: nothing of a window --
    // chrome, corners, shadow -- is drawn before its first present
    // (draw_one_window), so that is when all of it appears; the damage
    // its creation declared was spent on a frame that drew none of it.
    if (first)
        wm_damage_window_rect(win->x, win->y, win->w, win->h);
    else
        wm_damage_rect(window_content_x(win), window_content_y(win),
                        window_content_w(win), window_content_h(win));
    if (first) wm_anim_open(idx);
    redraw_pending = 1;
    wm_resize_shown(idx, 0);
}

// --- the client's widget map -----------------------------------------
//
// The client reports it; nothing here derives it. See
// abi/win_proto.h's WIN_REQ_WIDGET -- a compositor cannot see inside a
// window, so this is the client exporting its own tree the way an
// AT-SPI client does.
static void on_widget_reset(int pid, uint32_t id) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    struct window *w = &windows[idx];
    if (!w->widgets) {
        w->widgets = malloc(sizeof *w->widgets * WIN_WIDGET_MAX);
        if (!w->widgets) return;   // debug data; a machine short of memory
    }                              // has better uses for it than this
    w->widget_count = 0;
}

static void on_widget(int pid, uint32_t id, const struct wmchan_msg *m) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    struct window *w = &windows[idx];
    // NO RESET SEEN YET, so there is nowhere to put it. Dropping is
    // right: a map that began mid-batch would be a partial set claiming
    // to be whole.
    if (!w->widgets || w->widget_count >= WIN_WIDGET_MAX) return;
    struct wm_widget *e = &w->widgets[w->widget_count++];
    unsigned k = 0;
    while (m->text[k] && k < sizeof e->name - 1) { e->name[k] = m->text[k]; k++; }
    e->name[k] = '\0';
    e->x = m->a;
    e->y = m->b;
    e->w = WIN_WIDGET_W(m->c);
    e->h = WIN_WIDGET_H(m->c);
}

static void on_window_destroyed(int pid, uint32_t id) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    wm_anim_close(idx);   // the ghost snapshots the buffers BEFORE they are unmapped below

    // AN OWNER TAKES ITS DIALOGS WITH IT. The owner is re-found each
    // time round: close_window() renumbers windows[], so an index held
    // across one names whatever slid into that slot.
    for (;;) {
        int d = wm_dialog_of(idx, -1);
        if (d < 0) break;
        unmap_client_window(&windows[d]);
        windows[d].client_pid = 0;
        close_window(d);
        idx = find_client_window(pid, id);
        if (idx < 0) return;
    }

    // AND ITS POPUPS, for the same reason and by the same re-find: a
    // popup anchored to a window that is going away cannot be placed or
    // drawn against anything. Only reachable by destroying a toplevel
    // with one still open -- a client that DIES is swept whole, by pid
    // -- which is why nothing had noticed the row being left behind.
    for (;;) {
        int p = -1;
        for (int i = 0; i < window_count; i++)
            if (windows[i].popup && windows[i].client_pid == pid &&
                windows[i].popup_parent == id) { p = i; break; }
        if (p < 0) break;
        unmap_client_window(&windows[p]);
        windows[p].client_pid = 0;
        close_window(p);
        idx = find_client_window(pid, id);
        if (idx < 0) return;
    }

    // The pixels stay readable until this process lets go: the object
    // is the client's and this mapping holds it alive, whatever the
    // kernel's window table does. Dropping it here rather than later is
    // still right -- nothing composites a closed window.
    unmap_client_window(&windows[idx]);
    windows[idx].client_pid = 0;
    close_window(idx);
    wm_logf("wm: client pid %d closed window %u\n", pid, id);
}

// Stored and nothing else -- no damage: the sprite is not part of the
// scene the damage tracker describes, and wm.c redraws it from
// wm_cursor_shape_changed() instead.
static void on_window_cursor(int pid, uint32_t id, int cursor) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    windows[idx].client_cursor = cursor;
}

static void on_window_title(int pid, uint32_t id, const char *title) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    int i = 0;
    for (; title[i] && i < WIN_TITLE_MAX - 1; i++) windows[idx].title[i] = title[i];
    windows[idx].title[i] = '\0';

    // The title bar -- see on_window_present() on over-damaging.
    wm_damage_rect(windows[idx].x, windows[idx].y, windows[idx].w, WM_TITLEBAR_H);
    // AND THE TASKBAR STRIP, because the button carries this title too
    // (wm_taskbar.c's make_label). Damaging only the title bar left the
    // button showing the placeholder "Client" until something unrelated
    // repainted the strip -- every ring-3 window sets its real title
    // just after it opens, so this was the common path, not an edge.
    // Found by `gui damage verify on` once the client-content noise
    // stopped drowning it out (docs/decisions.md).
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);
    redraw_pending = 1;
}

// The client declaring how its window should behave. This is the other
// half of resizability living on the window (see wm.h): a client has no
// `gui_app` to carry it, so it says so itself.
static void on_window_hints(int pid, uint32_t id, unsigned flags, int min_w, int min_h) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    windows[idx].resizable = (flags & WIN_HINT_RESIZABLE) ? 1 : 0;
    windows[idx].scanout_ok = (flags & WIN_HINT_SCANOUT) ? 1 : 0;
    windows[idx].phys_keys = (flags & WIN_HINT_PHYS_KEYS) ? 1 : 0;
    windows[idx].min_w = min_w;
    windows[idx].min_h = min_h;

    // The resize grip appears or disappears with this, so the chrome
    // has to be repainted -- the frame only, not the content.
    wm_damage_window_rect(windows[idx].x, windows[idx].y, windows[idx].w, windows[idx].h);
    redraw_pending = 1;
}

// Defined below with the rest of the liveness code, which reads more
// naturally next to the ping that provokes it than up here.
static void on_window_pong(int pid, uint32_t id, uint32_t serial);

// The diagnostic channel's WM end (Milestone 41, stage 3). The `gui`
// commands arrive here as protocol messages now, instead of the serial
// console calling wm_debug_dispatch() directly across the kernel/apps
// boundary -- which is what has to stop before the WM can be a process.
//
// The command line is copied because wm_debug_dispatch_out() tokenises
// it IN PLACE, and what arrives is the message buffer.
//
// The ring-0 WM answered a `gui` command by being called back; a ring-3
// one is SENT the command and sends the OUTPUT BACK. That reply is not
// optional -- every GUI test tool reads it, so the desktop is
// unverifiable without it -- and it is the one path here that genuinely
// wants a round trip, because the console is waiting on the answer.
//
// Exported rather than deleted so the gap is stated once, here, instead
// of surfacing as an unused-function warning that reads like dead code.
// See docs/wm-ring3-design.md.
int wm_client_debug_command(const char *line, char *out, int cap) {
    char buf[DIAG_CMD_LEN];
    k_strlcpy(buf, line ? line : "", sizeof buf);

    struct dbg_out o = { .buf = out, .cap = cap, .len = 0, .overflow = 0 };
    if (!wm_debug_dispatch_out(buf, &o)) return -1;
    if (o.overflow) dbg_out_write(&o, "gui: (output truncated)\r\n");
    return o.len;
}

// Asks every window owned by `pid` to close, through the SAME
// wm_request_close() the X button, Alt+F4 and the context menu use --
// so a client may decline, and there is no fourth close path with its
// own idea of the rules.
//
// Iterates DOWNWARD because wm_request_close() may remove the window
// and shift everything above it down; going upward would skip the
// window that slid into the index just handled.
static int on_close_pid(int pid) {
    int asked = 0;
    for (int i = window_count - 1; i >= 0; i--) {
        if (i >= window_count) continue;      // the list shrank under us
        if (windows[i].client_pid != pid) continue;
        if (windows[i].popup) continue;       // asking a menu to close means nothing
        wm_request_close(i);
        asked++;
    }
    return asked;
}

// Raise the window carrying `app_id`. See WIN_REQ_ACTIVATE.
//
// This is the RAISE half of single-instance; the deciding half is in the
// client, which asks before it opens anything. So this deliberately says
// nothing about whether a second copy may run -- it answers "here it is"
// or "nobody there", and a caller that ignores the answer and opens a
// window anyway is behaving legally.
//
// Searches from the front so that if two windows somehow carry the same
// id (the create/ask race WIN_REQ_ACTIVATE documents, or two clients
// that simply chose the same name), the one the user saw most recently
// is the one that comes back.
// Raises windows[i] -- split out so the same raise can be entered from
// either end.
//
// In ring 0 the WM was asked "is there a window with this app id?", so
// it searched its own list. The KERNEL holds the app_ids now and answers
// that itself, sending the compositor the window it found -- which
// arrives as (pid, id), not as a name. One body with two doors, because
// two copies of a raise is two chances for the focus damage below to be
// right in only one of them.
static int raise_window_at(int i) {
    if (i < 0 || i >= window_count || !windows[i].open) return 0;

    // The window that is losing focus has to repaint its title bar,
    // exactly as on_window_created() and open_app() do -- otherwise
    // the old frontmost one keeps its focused blue until something
    // unrelated redraws it.
    if (window_count > 0 && i != window_count - 1) {
        struct window *losing = &windows[window_count - 1];
        wm_damage_window_rect(losing->x, losing->y, losing->w, losing->h);
    }

    wm_window_unminimize(i);
    bring_to_front(i);
    // bring_to_front() renumbers, so the window is at the top now --
    // damage it there rather than at the index just used.
    struct window *w = &windows[window_count - 1];
    wm_damage_window_rect(w->x, w->y, w->w, w->h);
    // A window dragged somewhere unreachable is exactly as useless
    // as no window at all, and this path is the only handle a
    // second launch gives the user -- same reasoning as the taskbar
    // button's, so it uses the same repair.
    wm_ensure_reachable(window_count - 1);
    redraw_pending = 1;
    return 1;
}

static int identity_for_pid(int pid);   // below, with the identity table

// **THE DECIDING HALF AND THE RAISING HALF ARE BOTH HERE NOW.** The
// kernel answered this while it held an identity per window; with that
// gone (stage 6a) the question is the compositor's again, and it is the
// same question the ring-0 WM used to answer -- only the identity comes
// from the process rather than from a string the client declared.
//
// Searches from the FRONT, so if two windows somehow share an identity
// the one the user saw most recently is the one that comes back. The
// asker's OWN windows are skipped: it is asking whether a TWIN exists,
// and matching itself would make every single-instance app refuse its
// own first window.
//
// Returns 1 if a twin was found and raised, 0 if nobody is there. The
// caller reads a 1 as "you are the second copy, exit quietly", so a
// wrong 1 is an app that never appears -- which is why an unknown
// identity (-1) matches nothing rather than everything.
static int activate_twin_of(int asking_pid) {
    int self = identity_for_pid(asking_pid);
    if (self < 0) return 0;
    for (int i = window_count - 1; i >= 0; i--) {
        if (!windows[i].open) continue;
        if (windows[i].client_pid == asking_pid) continue;
        if (windows[i].app_identity != self) continue;
        if (!raise_window_at(i)) continue;
        wm_logf("wm: raised the existing window of pid %d for pid %d\n",
                windows[i].client_pid, asking_pid);
        return 1;
    }
    return 0;
}

// The client arming or cancelling its repeating timer (WIN_REQ_TIMER).
// Milliseconds in, nanoseconds kept: the kernel's timers are one-shot
// deadlines now, so a 16 ms request is 16 ms rather than the next 10 ms
// tick after it.
static void on_window_timer(int pid, uint32_t id, unsigned ms) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    windows[idx].timer_period_ns = (uint64_t)ms * 1000000ull;
    windows[idx].timer_due_ns = ms ? sys_monotonic_ns() + windows[idx].timer_period_ns : 0;
}

// The earliest client timer deadline, or 0 if no window has one armed.
// The frame loop's wait must not outlast this, or a client that asked
// to be woken every 16 ms would be woken on the compositor's
// housekeeping cadence instead.
uint64_t wm_client_next_timer_due(void) {
    uint64_t soonest = 0;
    for (int i = 0; i < window_count; i++) {
        struct window *win = &windows[i];
        if (!win->open || !win->timer_period_ns) continue;
        if (!wm_client_is_client_window(win)) continue;
        if (!soonest || win->timer_due_ns < soonest) soonest = win->timer_due_ns;
    }
    return soonest;
}

// Once per frame: deliver WIN_EV_TIMER to every client whose interval
// has come round.
void wm_client_check_timers(void) {
    uint64_t now = sys_monotonic_ns();
    for (int i = 0; i < window_count; i++) {
        struct window *win = &windows[i];
        if (!win->open || !win->timer_period_ns) continue;
        if (!wm_client_is_client_window(win)) continue;
        if (now < win->timer_due_ns) continue;

        struct win_event ev = {0};
        ev.type = WIN_EV_TIMER;
        ev.window = win->client_win;
        if (!win_events_push(win->client_pid, &ev)) note_dropped(win);
        win->timer_due_ns += win->timer_period_ns;
        if (win->timer_due_ns <= now) win->timer_due_ns = now + win->timer_period_ns;
    }
}

// WM_SERVER_OPS is GONE. It was the kernel's way of calling INTO the WM,
// which a ring-3 process cannot be. Every slot it held is now an event
// the compositor receives -- see wm_client_handle_event() above.

// Claims the compositor role. Everything else the WM is allowed to do
// with the screen is gated on holding it -- the framebuffer grant
// (WIN_REQ_FB_MAP) is refused to anyone else, and raw input is
// delivered only to the holder.
//
// A REQUEST, not a registration. The ring-0 WM handed the kernel a
// `struct win_server_ops` and the kernel called back into it; ring 3
// cannot be called into, so the relationship inverts -- the WM claims
// the role and then RECEIVES what it used to be asked. See
// wm_rawin.c for the input half.
// --- client requests, received (M41 stage 4d) -------------------------
//
// The inbound half of the inversion. The kernel used to CALL a ring-0
// presentation layer's callbacks; a ring-3 compositor is TOLD instead,
// one WIN_EV_CLIENT_* event per callback, and reads back the detail it
// needs.
//
// Why the events are thin and this asks for the rest: struct win_event
// is 24 bytes and a title is 32, so carrying the detail inline would
// have meant widening every event in the protocol for the one that
// needs it. The kernel already HAS these facts (it received them), so
// it keeps them and answers WIN_REQ_WINDOW_INFO.


// query_app_id() IS GONE with WIN_REQ_WINDOW_APPID. The app id was read
// back from the kernel because a create EVENT is 24 bytes and could not
// carry it; a create MESSAGE can, so it arrives with the window.

// --- application identity ---------------------------------------------
//
// WHAT PROGRAM A CLIENT IS, which is the taskbar's grouping key and the
// whole of single instance. It comes from the process's SPAWN PATH
// (QUERY_PROCPATH), never from anything the client said: two apps
// declaring the same app_id would otherwise raise each other's windows,
// and a single-instance app told its twin is up exits without ever
// drawing.
//
// Interned to an int because both readers COMPARE it, and because the
// paths are FS_PATH_MAX and a window is not the place to keep 64 bytes
// of string. Never reclaimed: entries are program paths, of which a
// running system has a handful, and a refcount would exist to save
// nothing. Full means the next new program is ungrouped, which is a
// degradation rather than a failure.
#define WM_IDENTITY_MAX 32

static char g_ident_path[WM_IDENTITY_MAX][QUERY_PROCPATH_MAX];
static int  g_ident_count;

static int intern_identity(const char *path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < g_ident_count; i++)
        if (k_strcmp(g_ident_path[i], path) == 0) return i;
    if (g_ident_count >= WM_IDENTITY_MAX) {
        wm_logf("wm: identity table full -- window ungrouped\n");
        return -1;
    }
    k_strlcpy(g_ident_path[g_ident_count], path, QUERY_PROCPATH_MAX);
    return g_ident_count++;
}

// A LINEAR SCAN of the process list per call, and that is fine: it runs
// when a window is created and when a single-instance app starts, never
// per frame. -1 for a process the kernel has no path for, which matches
// nothing -- the safe direction for both readers.
static int identity_for_pid(int pid) {
    struct query_procpath r;
    QUERY_FOREACH(QUERY_PROCPATH, r, i) {
        if (r.pid == pid) return intern_identity(r.path);
    }
    return -1;
}

// Whether `pid` was spawned out of the screensaver directory. The same
// linear scan and the same rule as identity_for_pid() above: what a
// program IS comes from the path it was spawned from, never from
// anything it said about itself.
int wm_pid_is_screensaver(int pid) {
    struct query_procpath r;
    uint32_t n = (uint32_t)k_strlen(SCREENSAVER_DIR);
    QUERY_FOREACH(QUERY_PROCPATH, r, i) {
        if (r.pid != pid) continue;
        return k_strncmp(r.path, SCREENSAVER_DIR, n) == 0 && r.path[n] == '/';
    }
    return 0;
}

// Maps a client's buffer into this process, so the compositor can read
// its pixels. Idempotent, and must be re-done after a resize: the frames
// are reallocated, and the old mapping is revoked with them.
// --- TWP over a channel ----------------------------------------------
//
// See lib/uwmchan.h. A request that arrives here reached this process
// directly, carrying its payload -- no kernel copy and no read-back.
void wm_client_chan_open(void) {
    if (uchan_server_open(&g_chan, WMCHAN_SERVICE) < 0)
        wm_logf("wm: no client channel -- no client can open a window\n");
}

int wm_client_chan_ready(void) { return g_chan.beacon != 0; }

void wm_client_chan_wait(int timeout_ms) {
    // **ZERO MEANS DO NOT PARK, AND THE TWO CALLS BELOW DISAGREE ABOUT
    // THAT.** sys_wait_ready(0) returns at once; sys_futex_wait(w, v, 0)
    // means NO DEADLINE and parks until somebody wakes it. The WM asks
    // for 0 on every frame it already owes a repaint, so passing it
    // through parked the compositor until a client happened to send
    // something -- the desktop stalled, the watchdog fired, and init
    // restarted it in a loop.
    if (timeout_ms <= 0) return;

    // THE EVENT QUEUE IS CHECKED FIRST, and not parking when it has
    // something is the whole of what this adds over uchan_server_wait():
    // that call watches the rings and the wakeword, and an event queued
    // just before it would otherwise wait out the whole timeout.
    if (sys_wait_ready(0)) return;
    uchan_server_wait(&g_chan, timeout_ms);
}

// Drains everything queued. Called AFTER the event queue is pumped --
// see lib/uwmchan.h on why that order is load-bearing rather than
// tidy.
void wm_client_chan_pump(void) {
    if (!g_chan.beacon) return;

    // **THE SCAN IS HOW A DEAD CLIENT'S WINDOWS CLOSE.** A client's ring
    // is an shm object it created, and the kernel unlinks a dead
    // creator's objects -- so a name that no longer resolves IS the
    // death notification, arriving on the transport this process
    // already polls every frame. The kernel used to send an event,
    // which meant it had to know a window existed; it does not any more
    // (docs/winserver-ring3-design.md, stage 6b).
    //
    // A SMALL FIXED CAP, because this runs every frame: the scan keeps
    // a death that does not fit and reports it next frame.
    #define WM_CHAN_GONE_MAX 16
    int gone[WM_CHAN_GONE_MAX];
    int n = uchan_server_scan(&g_chan, gone, WM_CHAN_GONE_MAX);
    for (int i = 0; i < n; i++) {
        for (int w = window_count - 1; w >= 0; w--)
            if (windows[w].client_pid == gone[i])
                on_window_destroyed(gone[i], windows[w].client_win);
    }

    // BOUNDED, as wm_rawin_pump() is: clients refill their rings while
    // this drains them, and "until empty" can starve the frame. What is
    // left keeps the loop from parking (uchan_server_wait()).
    #define WM_CHAN_DRAIN_MAX 256
    int from;
    struct wmchan_msg m;
    for (int budget = WM_CHAN_DRAIN_MAX;
         budget > 0 && (from = uchan_server_recv(&g_chan, &m, sizeof m)) != 0; budget--) {
        m.text[sizeof m.text - 1] = '\0';
        switch (m.type) {
        // THE PAYLOAD IS HERE, which is the point: each of these took a
        // "something changed" event through the kernel and then a
        // WIN_REQ_WINDOW_INFO to read the detail back, because struct
        // win_event is 24 bytes and none of them fits.
        case WIN_REQ_TITLE:
            on_window_title(from, m.window, m.text);
            break;
        case WIN_REQ_NOTICE:
            crash_notice_piece(from, m.a, m.b, (unsigned)m.c, m.text);
            break;
        case WIN_REQ_HINTS:
            on_window_hints(from, m.window, (unsigned)m.a, m.b, m.c);
            break;
        case WIN_REQ_FULLSCREEN: {
            int idx = find_client_window(from, m.window);
            if (idx >= 0) wm_set_fullscreen(idx, m.a);
            break;
        }
        case WIN_REQ_CURSOR:
            on_window_cursor(from, m.window, m.a);
            break;
        case WIN_REQ_DRAG_START:
            wm_dnd_start(from, m.a);
            break;
        case WIN_REQ_DRAG_END:
            wm_dnd_end(from);
            break;
        // A CAPTURE ROUND-TRIPS, because the client has to know what it
        // got: a WINDOW capture's size is the compositor's answer, not
        // the client's question, and a refusal has to be told apart
        // from an unwritten buffer.
        case WIN_REQ_SCREENSHOT: {
            struct wmchan_msg r;
            k_memset(&r, 0, sizeof r);
            r.type = WIN_REQ_SCREENSHOT;
            r.shot = m.shot;
            r.a = wm_screenshot_capture(from, m.a, (unsigned)m.b, m.c, &r.shot);
            uchan_server_reply(&g_chan, from, &r, sizeof r);
            break;
        }
        // **CREATE IS THE ONLY REQUEST THAT ALLOCATES.** The client
        // proposes a slot -- its buffer objects are already named after
        // it -- and this accepts unless that (pid, slot) is already
        // open. Refusing is a -1 rather than silence: a client with no
        // answer would sit out its whole timeout and then open nothing,
        // which is the same outcome reached slowly.
        case WIN_REQ_CREATE: {
            struct wmchan_msg r;
            k_memset(&r, 0, sizeof r);
            r.type = WIN_REQ_CREATE;
            r.a = -1;
            if (find_client_window(from, m.window) < 0 &&
                on_window_created(from, m.window, m.a, m.b, 0, 0,
                                  m.text, identity_for_pid(from))) {
                r.a = (int)m.window;
                // A SAVER IS A SAVER WHOEVER STARTED IT (wm_idle.h), so
                // the Test button and a shell prompt get the same
                // dismiss-on-input the idle clock's own does.
                if (wm_pid_is_screensaver(from)) wm_idle_adopt_saver(from);
            }
            uchan_server_reply(&g_chan, from, &r, sizeof r);
            break;
        }
        // **THE FRAME BRINGS ITS OWN GEOMETRY**, which is what retired
        // WIN_REQ_RESIZE and WIN_REQ_BUFFER: there is no second record
        // of a buffer's size to keep in step, so there is none to go
        // stale. Ownership is checked the way every request here is --
        // `from` is the ring's pid and cannot be forged.
        case WIN_REQ_PRESENT:
            on_window_present(from, m.window, WIN_PRESENT_BUF(m.a),
                              WIN_PRESENT_GEN(m.a),
                              WIN_PRESENT_W((uint32_t)m.b),
                              WIN_PRESENT_H((uint32_t)m.b));
            break;
        case WIN_REQ_POPUP: {
            // The third round trip: the client draws its menu at the
            // answer, so it has to have one. Refused as -1 for a parent
            // that is not the sender's, a slot already open, or nothing
            // to map.
            struct wmchan_msg r;
            k_memset(&r, 0, sizeof r);
            r.type = WIN_REQ_POPUP;
            r.a = -1;
            int px = 0, py = 0;
            if (find_client_window(from, m.window) < 0 &&
                on_popup_created(from, m.window, (uint32_t)m.c, m.a, m.b,
                                 &m.pos, &px, &py)) {
                r.a = (int)m.window;
                r.b = px;
                r.c = py;
            }
            uchan_server_reply(&g_chan, from, &r, sizeof r);
            break;
        }
        case WIN_REQ_DIALOG: {
            // Answers, as CREATE and POPUP do: the client has already
            // made this slot's buffers and needs to know whether to
            // draw into them or release them again.
            struct wmchan_msg r;
            k_memset(&r, 0, sizeof r);
            r.type = WIN_REQ_DIALOG;
            r.a = -1;
            if (find_client_window(from, m.window) < 0 &&
                on_dialog_created(from, m.window, WIN_DIALOG_OWNER(m.c), m.a, m.b,
                                  (int)WIN_DIALOG_FLAGS(m.c), m.text))
                r.a = (int)m.window;
            uchan_server_reply(&g_chan, from, &r, sizeof r);
            break;
        }
        case WIN_REQ_DESTROY:
            on_window_destroyed(from, m.window);
            break;
        case WIN_REQ_WIDGET_RESET:
            on_widget_reset(from, m.window);
            break;
        case WIN_REQ_WIDGET:
            on_widget(from, m.window, &m);
            break;
        case WIN_REQ_TIMER:
            on_window_timer(from, m.window, (unsigned)m.a);
            break;
        case WIN_REQ_INHIBIT_SHORTCUTS:
            // **RESOLVED TO THE COMPOSITOR'S INDEX, not stored raw.**
            // `m.window` is the slot as the CLIENT numbers it; every
            // other handler here goes through find_client_window() for
            // exactly that reason, and comparing a client's id against
            // wm_focus_index() matches by accident or not at all.
            wm_shortcut_inhibit(find_client_window(from, m.window), m.a != 0);
            break;
        case WIN_REQ_PONG:
            on_window_pong(from, m.window, (uint32_t)m.a);
            break;
        case WIN_REQ_CLOSE_PID:
            // NOT about the sender's own window: it names another
            // process. Unprivileged, as it was through the kernel, and
            // strictly weaker than SYS_KILL -- every window it reaches
            // is ASKED, and may refuse.
            on_close_pid(m.a);
            break;
        case WIN_REQ_ACTIVATE: {
            // **THE ONE MESSAGE HERE THAT ANSWERS.** A single-instance
            // app asks this before it opens anything; the reply decides
            // whether it draws or exits, so it is always sent, including
            // the "nobody there" case -- a client left waiting spends
            // its whole timeout and then opens a duplicate.
            struct wmchan_msg r;
            k_memset(&r, 0, sizeof r);
            r.type = WIN_REQ_ACTIVATE;
            r.a = activate_twin_of(from);
            uchan_server_reply(&g_chan, from, &r, sizeof r);
            break;
        }
        default:
            // A message this build does not know. Dropped rather than
            // guessed at -- a client speaking a later protocol is not
            // an error the compositor can fix.
            break;
        }
    }
}

// --- a client's pixels, opened by NAME --------------------------------
//
// The kernel does not map a window into this process any more: the
// buffer is an shm object its client created and granted to us, and
// this opens it like any other. Nothing can be revoked underneath a
// mapping made here, which is what retired the poison page -- the
// object stays alive until this process munmaps it, however dead its
// window is.

// Drops one buffer's mapping. **THE MAPPING IS WHAT KEEPS A DEAD
// WINDOW'S FRAMES ALIVE**, so skipping it leaks them for as long as
// this process runs -- visible as a stranded row in `lsshm`.
static void unmap_buf(struct window *win, int b) {
    if (!win->client_mapped[b]) return;
    // PAGE-ROUNDED, because SYS_MUNMAP refuses anything else and a
    // window is w * h * 4 -- which is almost never a whole number of
    // pages. The same trap cost the client side a leaked buffer per
    // resize; `client_bytes` is the rounded length that was mapped.
    sys_munmap(win->client_px[b], win->client_bytes[b]);
    win->client_px[b] = 0;
    win->client_bytes[b] = 0;
    win->client_mapped[b] = 0;
}

static void unmap_client_window(struct window *win) {
    for (int b = 0; b < WIN_CLIENT_BUFS; b++) unmap_buf(win, b);
    win->client_buf = 0;
}

// Makes sure buffer `b` of this window is mapped at generation `gen`,
// big enough for `w` x `h`. A different generation means the client
// replaced the object behind the name, so the old mapping is dropped
// and the name re-opened.
//
// **THE SIZE IS CHECKED BY MAPPING IT.** The kernel takes a client's
// word for how big its buffer is -- it does not hold the object -- so a
// claim larger than the object would walk this process off the end of
// it. Asking mmap for exactly the claimed extent puts the check in the
// one place that knows both numbers: SYS_MMAP refuses a length past an
// shm object's own pages (-EINVAL), so a lying client costs its own
// window a frame and nothing else.
//
// **THE OLD MAPPING GOES ONLY ONCE THE NEW ONE IS IN.** `b` can be the
// slot on screen, and client_buf points into it: unmapped first, a
// failed replacement left the renderer blitting from a hole.
static int map_buf(struct window *win, int b, uint32_t gen, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ULL;
    if (win->client_mapped[b] && win->client_gen[b] == gen
        && win->client_bytes[b] >= bytes) return 1;

    char nm[WIN_BUF_NAME_MAX];
    k_snprintf(nm, sizeof nm, WIN_BUF_NAME_FMT,
               win->client_pid, (int)win->client_win, b);
    int fd = sys_shm_open(nm, 0, 0);
    if (fd < 0) return 0;
    // READ-ONLY, as the kernel's mapping was: a compositor composites
    // OUT of a client buffer and never writes one, so a write here is a
    // bug worth faulting on rather than a client's pixels quietly
    // changing under it.
    void *p = sys_mmap(0, bytes, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) {
        // SAY WHICH FAILURE. "does not fit its object" is only true of
        // the oversize case (-EINVAL); the other one is the mapping
        // TABLE being full (-ENOMEM), where the object is exactly the
        // right size and nothing about this window is wrong. Reporting
        // the first for the second sent a session looking at buffer
        // sizes while the compositor was out of regions.
        wm_logf("wm: pid %d window %u buffer %d: cannot map %dx%d (%s)\n",
                win->client_pid, win->client_win, b, w, h,
                "no free mapping slot, or the object is the wrong size");
        return 0;
    }

    unmap_buf(win, b);
    win->client_px[b] = (uint32_t *)p;
    win->client_bytes[b] = bytes;
    win->client_gen[b] = gen;
    win->client_mapped[b] = 1;
    if (b == win->client_front && win->client_buf) win->client_buf = win->client_px[b];
    return 1;
}

// One event in, one callback out. Returns 1 if the event was a client
// request this handled, so the caller can tell it apart from raw input.
// The `gui` command channel, POLLED once per frame rather than driven
// by an event -- and the reason is worth keeping.
//
// It WAS an event (WIN_EV_CLIENT_DEBUG), and that worked perfectly right
// up until the first client window existed. A client presents every
// frame, each present is an event, and the compositor's queue is 32 deep
// and drops the OLDEST when it overflows -- so a busy client silently
// flooded the debug event out of the queue and the console timed out
// forever, while the desktop carried on drawing perfectly.
//
// That is not something to tune away with a bigger queue. The diagnostic
// channel is how all 22 GUI test tools reach the WM, so it must not
// share a lossy queue with ordinary traffic whose rate a CLIENT
// controls. Polling costs one request per frame and cannot be starved.
//
// DIAG_TAKE answers 0 when nothing is pending, which is the common case
// and the whole cost.
//
// **THE COMPOSITOR IS A REGISTERED PROVIDER NAMED `gui`**, not a
// hardwired endpoint -- see abi/diag_abi.h. The claim is made here on
// the first poll rather than at startup, so it survives the kernel
// forgetting it (a provider is dropped when its pid dies, and a
// restarted desktop is a new pid).
void wm_client_poll_debug(void) {
    static int claimed;
    if (!claimed) {
        struct diag_msg c;
        k_memset(&c, 0, sizeof c);
        c.type = DIAG_CLAIM;
        k_strlcpy(c.name, "gui", DIAG_NAME_LEN);
        claimed = sys_diag(&c) == 1;
        if (!claimed) return;
    }

    struct diag_msg q;
    k_memset(&q, 0, sizeof q);
    q.type = DIAG_TAKE;
    if (sys_diag(&q) != 1) return; // nothing waiting

    static char reply[DIAG_REPLY_MAX];
    int n = wm_client_debug_command(q.text, reply, sizeof reply);

    struct diag_msg r;
    if (n < 0) {
        // Unrecognised, which a caller must be able to tell from a
        // command that legitimately printed nothing.
        k_memset(&r, 0, sizeof r);
        r.type = DIAG_REPLY;
        r.flags = DIAG_F_UNKNOWN;
        r.len = 0;
        sys_diag(&r);
        return;
    }

    // Chunked: one message carries DIAG_CHUNK bytes and a `gui windows
    // --json` is routinely longer. DIAG_F_MORE on every piece but the
    // last, which is what releases the waiting caller.
    int sent = 0;
    do {
        int piece = n - sent;
        if (piece > DIAG_CHUNK) piece = DIAG_CHUNK;
        k_memset(&r, 0, sizeof r);
        r.type = DIAG_REPLY;
        for (int i = 0; i < piece; i++) r.text[i] = reply[sent + i];
        r.text[piece] = '\0';
        r.len = (uint32_t)piece;
        sent += piece;
        if (sent < n) r.flags = DIAG_F_MORE;
        sys_diag(&r);
    } while (sent < n);
}

int wm_client_handle_event(const struct win_event *ev) {
    if (!ev) return 0;

    switch (ev->type) {
    // **NO WIN_EV_CLIENT_* ARM SURVIVES.** Every request a client makes
    // of this process arrives on its own channel ring now, carrying its
    // payload (lib/uwmchan.h) -- create, present, destroy, title, hints,
    // cursor, timer, pong, close-pid and activate alike. What the
    // kernel's queue still carries is what the KERNEL owns: raw input,
    // the font moving, the screen changing mode, and a `gui` command.
    case WIN_EV_FONT:
        // The metrics moved under us. Everything this compositor draws
        // is derived from ugfx_char_w()/ugfx_char_h() FRESH each frame
        // (chrome, the taskbar, the icon grid, menu rows), so re-mapping
        // the font and forcing one full repaint is the whole job -- no
        // cached geometry to invalidate, which is a property worth not
        // losing. wm_render_reset() is what makes the next frame
        // unconditional rather than damage-limited.
        ugfx_font_init();
        wm_render_reset();
        wm_logf("wm: font changed -- %dx%d cell\n", ugfx_char_w(), ugfx_char_h());
        // And every client's, which the kernel told only this process.
        broadcast_state(WIN_EV_FONT, 0, 0, WM_PEND_FONT);
        break;
    case WIN_EV_SCREEN:
        wm_screen_changed();
        broadcast_state(WIN_EV_SCREEN, screen_w, screen_h, WM_PEND_SCREEN);
        break;
    // Pushed configuration: move a topic's counter and let its poller
    // re-read on the next frame (wm_watch.h).
    case WIN_EV_FSWATCH:
        wm_watch_fired(ev->a);
        break;
    case WIN_EV_SETTING:
        wm_watch_setting();
        // Forwarded, because the kernel tells only this process: an app
        // showing a date re-reads the zone and the region (ui/uapp.c).
        broadcast_state(WIN_EV_SETTING, 0, 0, WM_PEND_SETTING);
        break;
    default:
        return 0; // not ours -- raw input, see wm_rawin.c
    }
    return 1;
}

int wm_claim_compositor(void) {
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 1; // claim
    return sys_win_request(&req) == 1;
}

void wm_client_init(void) {
    // WM_SERVER_OPS is not registered any more -- see wm_claim_compositor()
    // above. The callbacks it named become events the WM receives; the
    // table itself is what stage 4c's remaining work replaces.
}

void wm_client_shutdown(void) {
    // Released on the way out so a client request made after the desktop
    // has exited is refused, rather than dispatched into a window list
    // nobody is drawing. Dropping the role also revokes the framebuffer
    // grant -- the kernel does that wherever the role is cleared, which
    // is one place, so a clean exit, a kill and a fault are the same
    // path (see kernel/proc/win_surface.c).
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 0; // release
    sys_win_request(&req);
}

int wm_client_is_client_window(const struct window *win) {
    return win && win->client_pid != 0 && win->client_buf != 0;
}

void wm_client_draw(const struct window *win) {
    if (!wm_client_is_client_window(win)) return;
    ugfx_blit(wm_surface(), window_content_x(win), window_content_y(win),
              win->client_w, win->client_h, win->client_buf, win->client_w);
}

// --- input routing ----------------------------------------------------
//
// The WM decides WHO an event belongs to (focus, hit-testing, z-order)
// and the protocol decides WHAT it says. These two functions are the
// whole of the first half for clients: everything else about routing --
// which window is frontmost, whether the click landed on chrome -- is
// wm_input.c's existing logic, unchanged.

void wm_client_send_key(struct window *win, int key, unsigned mods) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_KEY;
    ev.window = win->client_win;
    ev.a = key;
    ev.mods = mods;
    if (!win_events_push(win->client_pid, &ev)) note_dropped(win);
}

// The other edge. A separate function rather than a `down` flag on the
// one above, because the two have different ROUTING rules a few lines up
// in wm.c -- a press can be claimed by the WM as a shortcut and a
// release never is -- and a flag would invite a caller to pass the wrong
// one at a site that had not thought about which.
void wm_client_send_key_up(struct window *win, int key, unsigned mods) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_KEY_UP;
    ev.window = win->client_win;
    ev.a = key;
    ev.mods = mods;
    if (!win_events_push(win->client_pid, &ev)) note_dropped(win);
}

// A key BY POSITION (abi/win_proto.h's WIN_EV_KEY_PHYS), to the focused
// window if it asked for them. **A PRESS WHILE AN OVERLAY IS OPEN IS NOT
// ITS**: the Start menu is being typed into, and a game behind it must
// not walk. A release always goes, or a key held as the menu opened
// would stay down in the game.
void wm_client_route_phys_key(int keycode, int down, unsigned mods) {
    int f = wm_focus_index();
    if (f < 0 || !wm_client_is_client_window(&windows[f]) || !windows[f].phys_keys) return;
    if (down && wm_overlay_any_open()) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_KEY_PHYS;
    ev.window = windows[f].client_win;
    ev.a = keycode;
    ev.b = down;
    ev.mods = mods;
    if (!win_events_push(windows[f].client_pid, &ev)) note_dropped(&windows[f]);
}

void wm_client_send_mouse(struct window *win, int type, int x, int y, unsigned buttons) {
    if (!wm_client_is_client_window(win)) return;

    // A move to where the cursor already is carries no information.
    // Dropping it matters more than it sounds: wm_update_content_hover()
    // runs every frame, so without this a motionless cursor wakes the
    // client at frame rate forever -- and under the scheduler's
    // rotation a client woken every frame takes half the CPU to decide
    // nothing changed. Presses and releases are never suppressed; only
    // redundant motion is.
    if (type == WIN_EV_MOUSE_MOVE) {
        if (x == win->client_last_mx && y == win->client_last_my) return;
        win->client_last_mx = x;
        win->client_last_my = y;
    }
    struct win_event ev = {0};
    ev.type = (uint32_t)type;
    ev.window = win->client_win;
    // Window-relative, not screen coordinates: a client knows nothing
    // about where the WM put it, and telling it would make every client
    // re-derive the same subtraction.
    ev.a = x - window_content_x(win);
    ev.b = y - window_content_y(win);
    // The buttons in the low byte, the LIVE keyboard modifiers in the
    // next one -- see WIN_MOUSE_MODS_SHIFT. Read here rather than passed
    // in because every caller would otherwise read the same global.
    ev.mods = WIN_MOUSE_BUTTONS(buttons) |
              ((unsigned)wm_rawin_pointer_mods() << WIN_MOUSE_MODS_SHIFT);
    if (!win_events_push(win->client_pid, &ev)) {
        note_dropped(win);
        // So the next motion is not read as "already delivered".
        if (type == WIN_EV_MOUSE_MOVE) win->client_last_mx = INT32_MIN;
    }
}

// Propose a new CONTENT size. Deliberately a proposal: the WM does not
// resize a client's window, because the buffer belongs to the client
// and only it can decide when that changes. If the client agrees it
// answers with WIN_REQ_RESIZE and on_window_resized() above adopts the
// result; if it ignores this, nothing happens and the window stays as
// it was. Same politeness as the close button.
void wm_client_send_scanout(struct window *win, int on, uint32_t pitch, int count, int back) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_SCANOUT;
    ev.window = win->client_win;
    ev.a = on ? 1 : 0;
    ev.b = (int32_t)pitch;
    ev.mods = (uint32_t)(count & 0xFF) | ((uint32_t)back << 8);
    if (!win_events_push(win->client_pid, &ev)) note_dropped(win);
}

void wm_client_send_resize(struct window *win, int w, int h) {
    if (!wm_client_is_client_window(win)) return;

    int min_w = win->min_w > 0 ? win->min_w : 1;
    int min_h = win->min_h > 0 ? win->min_h : 1;
    if (w < min_w) w = min_w;
    if (h < min_h) h = min_h;

    struct win_event ev = {0};
    ev.type = WIN_EV_RESIZE;
    ev.window = win->client_win;
    ev.a = w;
    ev.b = h;
    win->ev_resize_w = w;
    win->ev_resize_h = h;
    send_state(win, &ev, WM_PEND_RESIZE);
}

// Keyboard focus arrived or left. Only a client needs telling: a
// kernel-space app is called by the WM, which knows perfectly well
// which window is frontmost, whereas a client sees nothing but its own
// event queue.
void wm_client_send_focus(struct window *win, int focused) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_FOCUS;
    ev.window = win->client_win;
    ev.a = focused ? 1 : 0;
    send_state(win, &ev, WM_PEND_FOCUS);
}

// Wheel notches, to the focused client. Same focus rule as keys: who
// receives it is the WM's decision and does not change because the
// recipient is a process rather than a callback.
void wm_client_send_wheel(struct window *win, int notches) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_WHEEL;
    ev.window = win->client_win;
    ev.a = notches;
    if (!win_events_push(win->client_pid, &ev)) note_dropped(win);
}

void wm_client_send_close(struct window *win) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_CLOSE;
    ev.window = win->client_win;
    send_state(win, &ev, WM_PEND_CLOSE);

    // Ask whether it is even listening, at the same moment. A close that
    // goes unanswered means one of two very different things -- the app
    // declined, or the app is wedged -- and only the ping can tell them
    // apart. Getting that wrong in either direction is bad: offering to
    // force-quit an app that deliberately refused is obnoxious, and
    // refusing to offer it for one that is hung is the whole problem.
    win->close_asked_tick = sys_ticks();
    wm_client_ping(win);
}

// ---------------------------------------------------------------------
// Is it still there?
// ---------------------------------------------------------------------
//
// An unresponsive client is invisible from out here. It holds its
// window, its buffer stays mapped, and it sends nothing -- which is
// also an exact description of a client that is idle and perfectly
// healthy. Nothing the WM can observe separates them.
//
// So it asks. WIN_EV_PING carries a serial, the client's event loop
// echoes it back in WIN_REQ_PONG (uapp does this, so no application
// contains ping code), and a client that does not answer within
int wm_ping_timeout_ticks = WM_PING_TIMEOUT_DEFAULT;
int wm_ping_interval_ticks = WM_PING_INTERVAL_DEFAULT;

// wm_ping_timeout_ticks is not answering its queue at all. That is
// xdg_shell's ping and ICCCM's _NET_WM_PING, for the same reason.
//
// The serial is not decoration: without it a late pong from a previous
// ping would clear the current one, so an app answering every check one
// round behind -- exactly what a badly overloaded app does -- would
// always look healthy.

static uint32_t g_next_serial = 1;

void wm_client_ping(struct window *win) {
    if (!wm_client_is_client_window(win)) return;
    if (win->ping_serial) return; // one outstanding at a time

    if (++g_next_serial == 0) g_next_serial = 1; // 0 means "none"

    struct win_event ev = {0};
    ev.type = WIN_EV_PING;
    ev.window = win->client_win;
    ev.a = (int)g_next_serial;
    // A ping the inbox could not take was never asked, so no serial is
    // left outstanding and the liveness check asks again. How long it
    // has been refused is kept: a client that never drains its inbox is
    // as hung as one that never answers (wm_client_check_liveness()).
    if (!win_events_push(win->client_pid, &ev)) {
        if (!win->ping_blocked_tick) win->ping_blocked_tick = sys_ticks();
        return;
    }
    win->ping_blocked_tick = 0;
    win->ping_serial = g_next_serial;
    win->ping_sent_tick = sys_ticks();
    win->ping_sent_ns = sys_monotonic_ns();
    win->ping_sent_tsc = __builtin_ia32_rdtsc();
}

static unsigned long long g_ping_last_us, g_ping_max_us, g_ping_sum_us;
static unsigned long long g_ping_last_cyc, g_ping_max_cyc, g_ping_sum_cyc;
static unsigned g_ping_n;
// The same round trip as a distribution, and resettable -- the running
// average above is lifetime-of-boot, so it cannot answer "before and
// after the load started" within one session.
static struct wmwd_dist g_ping_dist;

void wm_client_ping_stats(unsigned long long *last_us, unsigned long long *max_us,
                          unsigned long long *avg_us, unsigned *n) {
    *last_us = g_ping_last_us;
    *max_us = g_ping_max_us;
    *avg_us = g_ping_n ? g_ping_sum_us / g_ping_n : 0;
    *n = g_ping_n;
}

const struct wmwd_dist *wm_client_ping_dist(void) { return &g_ping_dist; }

void wm_client_ping_reset(void) {
    struct wmwd_dist zero = {0};
    g_ping_dist = zero;
    g_ping_last_us = g_ping_max_us = g_ping_sum_us = 0;
    g_ping_last_cyc = g_ping_max_cyc = g_ping_sum_cyc = 0;
    g_ping_n = 0;
}

void wm_client_ping_cycles(unsigned long long *last, unsigned long long *max,
                           unsigned long long *avg) {
    *last = g_ping_last_cyc;
    *max = g_ping_max_cyc;
    *avg = g_ping_n ? g_ping_sum_cyc / g_ping_n : 0;
}

static void on_window_pong(int pid, uint32_t id, uint32_t serial) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    struct window *w = &windows[idx];
    if (serial != w->ping_serial) return; // stale -- see above
    w->ping_serial = 0;
    unsigned long long rtt = (sys_monotonic_ns() - w->ping_sent_ns) / 1000;
    unsigned long long cyc = __builtin_ia32_rdtsc() - w->ping_sent_tsc;
    g_ping_last_us = rtt;
    if (rtt > g_ping_max_us) g_ping_max_us = rtt;
    g_ping_sum_us += rtt;
    g_ping_last_cyc = cyc;
    if (cyc > g_ping_max_cyc) g_ping_max_cyc = cyc;
    g_ping_sum_cyc += cyc;
    g_ping_n++;
    wmwd_dist_add(&g_ping_dist, rtt);
    if (w->not_responding) {
        w->not_responding = 0;
        // The title bar said "(Not Responding)": DAMAGE it, do not just
        // ask for a frame. A frame that also carries other damage is
        // limited to that damage, and the title text then changed
        // outside it -- damage_sweep.py caught it under a ghost.
        wm_damage_rect(w->x, w->y, w->w, WM_TITLEBAR_H);
        redraw_pending = 1;
    }
}

// Called once per frame from wm_run(). Returns the index of a window
// that has just been found unresponsive WHILE BEING ASKED TO CLOSE, or
// -1 -- which is the only case that warrants interrupting the user.
int wm_client_check_liveness(void) {
    uint64_t now = sys_ticks();
    int report = -1;

    for (int i = 0; i < window_count; i++) {
        struct window *w = &windows[i];
        if (!wm_client_is_client_window(w)) continue;
        if (!w->ping_serial) {
            // ASK, on a cadence. ping_sent_tick doubles as "when the
            // last one went out" -- it outlives the serial the pong
            // clears -- so the interval is measured from the last ASK,
            // not the last answer.
            if (now - w->ping_sent_tick >= (uint64_t)wm_ping_interval_ticks)
                wm_client_ping(w);
            // AN INBOX FULL FOR A WHOLE TIMEOUT is a client not reading
            // it: hung, though no ping could be outstanding to time out.
            if (!w->ping_blocked_tick ||
                now - w->ping_blocked_tick < (uint64_t)wm_ping_timeout_ticks) continue;
        } else if (now - w->ping_sent_tick < (uint64_t)wm_ping_timeout_ticks) {
            continue;
        }

        if (!w->not_responding) {
            w->not_responding = 1;
            wm_damage_rect(w->x, w->y, w->w, WM_TITLEBAR_H); // the title text changes
            redraw_pending = 1;
            wm_logf("wm: client pid %d is not responding\r\n", w->client_pid);
        }
        // Only a window the user has actually asked to close earns a
        // dialog, and only once per ask. An app that hangs while nobody
        // is trying to do anything with it gets the title-bar mark and
        // nothing more -- a modal appearing on its own, over whatever
        // the user was doing, for a window they never touched, would be
        // worse than the hang.
        if (w->close_asked_tick &&
            w->close_asked_tick != w->force_quit_offered_tick) {
            w->force_quit_offered_tick = w->close_asked_tick;
            report = i;
        }
    }
    return report;
}
