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
// produced desktop.c/start_menu.c/file_picker.c. See wm.c's top
// comment.
#include "wm_internal.h"
#include "lib/uchan.h"
#include "lib/uwmchan.h"
#include "wm_geometry.h"
#include "wm_rawin.h"
#include "wm_debug.h" // the diagnostic channel's WM end, below
#include "win_server.h"
#include "kapi.h"
#include "ui/utheme.h"
#include "rt/sys.h"
#include "wm/wm_log.h"

// --- delivering events to clients (M41 stage 4c) ----------------------
//
// The ring-0 WM called win_events_push() -- a kernel function -- to put
// an event on a client's queue. A process cannot do that, so it ASKS:
// WIN_REQ_EVENT_PUSH, refused to anyone but the registered compositor,
// because this is the one request that reaches across into another
// process's queue.
//
// Same signature as the kernel function it replaces, so every call site
// is unchanged. Returns 1 on success, 0 if the request was refused or
// the target's queue is full -- and a full queue is not something the
// compositor can fix (the client is not draining), so it is reported
// rather than retried.
static int win_events_push(int pid, const struct win_event *ev) {
    if (!ev) return 0;
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_EVENT_PUSH;
    req.a = pid;
    req.window = ev->window;
    req.b = (int32_t)ev->type;
    req.c = ev->a;
    req.d = ev->b;
    req.mods = ev->mods;
    return sys_win_request(&req) == 0;
}

// Finds the windows[] slot for one client window, or -1. Linear over at
// most MAX_WINDOWS entries, which is nothing, and it avoids caching an
// index that close_window() reshuffles (see wm.c -- indices move when a
// window closes, which has bitten pending_write/pending_read/
// pending_proc for exactly this reason).
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

static int on_window_created(int pid, uint32_t id, uint32_t *buf,
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
        wm_damage_rect(losing_focus->x, losing_focus->y,
                        losing_focus->w, losing_focus->h);
    }

    struct window *win = &windows[window_count];
    k_memset(win, 0, sizeof(*win));

    // A client asking for (0,0) gets the same cascade an app window
    // would, rather than every client stacking exactly on the origin.
    int cascade = (window_count % 5) * 24;
    win->x = (x > 0) ? x : 60 + cascade;
    win->y = (y > 0) ? y : 40 + cascade;
    win->w = w + 2;
    win->h = h + WM_TITLEBAR_H + 2;
    win->state = WIN_NORMAL;
    win->app = 0;        // a client window has no gui_app -- see wm.h
    win->open = 1;
    win->client_pid = pid;
    win->client_win = id;
    win->client_buf = buf;
    win->client_base = buf;   // front is 0 until the first present
    win->client_front = 0;
    win->client_w = w;
    win->client_h = h;
    win->client_last_mx = INT32_MIN; // nothing delivered yet
    win->client_last_my = INT32_MIN;

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

    // A new window steals focus, so the one losing it has to be told.
    // (The new window itself does not need an event: a client that has
    // just created a window and been made frontmost can assume it has
    // focus, and uapp starts in that state.)
    if (window_count > 0) wm_client_send_focus(&windows[window_count - 1], 0);

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

// `front` is which of the window's two buffers now holds finished
// pixels -- the kernel flipped it inside WIN_REQ_PRESENT, before this
// event was queued, so by the time this runs the client is already
// drawing into the other one.
static void on_window_present(int pid, uint32_t id, int front, int w, int h) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    struct window *win = &windows[idx];

    // Point at the finished buffer. A single-buffered window (its
    // second allocation failed) always reports 0, so this is a no-op
    // there and the old behaviour is preserved exactly.
    win->client_front = front;
    if (win->client_base)
        win->client_buf = win->client_base + (front ? WIN_BUFFER_HALF / 4 : 0);

    // THE FRAME BRINGS ITS OWN SIZE, and this is where a resize lands.
    // Adopting it when the client ACCEPTED the proposal instead would
    // put the chrome around a buffer with nothing in it yet -- a whole
    // window of black for the length of the client's repaint. See
    // abi/win_proto.h's configure/ack.
    if (w > 0 && h > 0 && (w != win->client_w || h != win->client_h)) {
        wm_damage_rect(win->x, win->y, win->w, win->h);   // the rect being left
        win->client_w = w;
        win->client_h = h;
        win->w = w + 2;
        win->h = h + WM_TITLEBAR_H + 2;
        wm_damage_rect(win->x, win->y, win->w, win->h);
        redraw_pending = 1;
        // An interactive resize sends its next proposal now: one FRAME
        // in flight at a time, so the client's real repaint sets the
        // pace rather than its syscall latency.
        wm_resize_shown(idx, 1);
        return;
    }

    // Damage only the CONTENT area, not the whole window: the chrome
    // hasn't changed, and over-damaging is how a compositor quietly
    // stops being a compositor.
    wm_damage_rect(window_content_x(win), window_content_y(win),
                    window_content_w(win), window_content_h(win));
    redraw_pending = 1;
    wm_resize_shown(idx, 0);
}

static void on_window_destroyed(int pid, uint32_t id) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    // Drop the pointer BEFORE close_window() so nothing can composite
    // from a buffer whose frames are about to be freed -- win_server.c
    // frees them the moment this returns.
    windows[idx].client_buf = 0;
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
    windows[idx].min_w = min_w;
    windows[idx].min_h = min_h;

    // The resize grip appears or disappears with this, so the chrome
    // has to be repainted -- the frame only, not the content.
    wm_damage_rect(windows[idx].x, windows[idx].y, windows[idx].w, windows[idx].h);
    redraw_pending = 1;
}

// The client ACCEPTED a WIN_EV_RESIZE proposal: its back buffer is now
// this size and its frames moved, so the mapping is re-taken here.
//
// **THE SIZE IS NOT ADOPTED HERE.** What is on screen is still the front
// buffer at the old size, and it stays that way until the client has
// drawn the new one -- on_window_present() above does the adopting, with
// the pixels in hand. Doing it here is what made a resize flash black.
static void on_window_resized(int pid, uint32_t id, uint32_t *buf, int w, int h) {
    (void)w; (void)h;
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    struct window *win = &windows[idx];

    win->client_base = buf;
    win->client_buf = buf + (win->client_front ? WIN_BUFFER_HALF / 4 : 0);
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
// it IN PLACE, and what arrives is the transport's message buffer.
// NOT static, and not yet called from anywhere -- the one piece of the
// inversion still missing.
//
// In ring 0 the kernel calls this through win_server_ops when a `gui`
// command arrives over the serial console. A ring-3 WM has to be sent
// the command and send the OUTPUT BACK, and unlike every other callback
// here that reply is not optional: all 22 GUI test tools read it, so
// the desktop is unverifiable without it. It is also the only remaining
// path that genuinely wants a round trip, because the console is waiting
// on the answer.
//
// Exported rather than deleted so the gap is stated once, here, instead
// of surfacing as an unused-function warning that reads like dead code.
// See docs/wm-ring3-design.md.
int wm_client_debug_command(const char *line, char *out, int cap) {
    char buf[WIN_DEBUG_CMD_LEN];
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
        wm_damage_rect(losing->x, losing->y, losing->w, losing->h);
    }

    if (windows[i].state == WIN_MINIMIZED) windows[i].state = WIN_NORMAL;
    bring_to_front(i);
    // bring_to_front() renumbers, so the window is at the top now --
    // damage it there rather than at the index just used.
    struct window *w = &windows[window_count - 1];
    wm_damage_rect(w->x, w->y, w->w, w->h);
    // A window dragged somewhere unreachable is exactly as useless
    // as no window at all, and this path is the only handle a
    // second launch gives the user -- same reasoning as the taskbar
    // button's, so it uses the same repair.
    wm_ensure_reachable(window_count - 1);
    redraw_pending = 1;
    return 1;
}

// on_window_activate(const char *app_id) is GONE. It searched this WM's
// own window list by app id -- the question the KERNEL answers now. It
// received every app_id at create time and keeps them, so it finds the
// twin itself and sends the window it found. What is left for the
// compositor is the action, below.

// The ring-3 door: the kernel already decided WHICH window, so this only
// has to find it in the list and raise it.
static int on_window_activate_window(int pid, uint32_t id) {
    int i = find_client_window(pid, id);
    if (i < 0 || !raise_window_at(i)) return 0;
    wm_logf("wm: activated existing window (pid %d, window %u)\n", pid, id);
    return 1;
}

// The client arming or cancelling its repeating timer (WIN_REQ_TIMER).
//
// Milliseconds in, ticks out, floored at ONE: a client asking for a
// faster interval than the timer resolution gets "every tick" rather
// than a refusal, and -- more importantly -- rather than an interval of
// zero, which would make the due-check below fire on every single frame
// and turn a request to slow down into the busiest possible loop.
static void on_window_timer(int pid, uint32_t id, unsigned ms) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    if (ms == 0) {
        windows[idx].timer_ticks = 0;
        windows[idx].timer_due = 0;
        return;
    }

    unsigned ticks = (ms * PIT_HZ) / 1000;
    if (ticks == 0) ticks = 1;
    windows[idx].timer_ticks = ticks;
    windows[idx].timer_due = sys_ticks() + ticks;
}

// Once per frame: deliver WIN_EV_TIMER to every client whose interval
// has come round.
//
// The next deadline is computed from NOW, not by adding the interval to
// the old one. Those differ only when a client is slower than its own
// timer -- and there the second form quietly builds a queue of overdue
// firings that all arrive at once the moment it catches up, which is
// the opposite of what a client asking to be woken less often wanted.
// The earliest client timer deadline, in ticks, or 0 if no window has
// one armed. The frame loop's wait must not outlast this, or a client
// that asked to be woken every 16 ms would be woken on the compositor's
// housekeeping cadence instead -- which is the timer service quietly
// becoming slower than the timers it serves.
uint64_t wm_client_next_timer_due(void) {
    uint64_t soonest = 0;
    for (int i = 0; i < window_count; i++) {
        struct window *win = &windows[i];
        if (!win->open || !win->timer_ticks) continue;
        if (!wm_client_is_client_window(win)) continue;
        if (!soonest || win->timer_due < soonest) soonest = win->timer_due;
    }
    return soonest;
}

void wm_client_check_timers(void) {
    uint64_t now = sys_ticks();
    for (int i = 0; i < window_count; i++) {
        struct window *win = &windows[i];
        if (!win->open || !win->timer_ticks) continue;
        if (!wm_client_is_client_window(win)) continue;
        if (now < win->timer_due) continue;

        struct win_event ev = {0};
        ev.type = WIN_EV_TIMER;
        ev.window = win->client_win;
        win_events_push(win->client_pid, &ev);
        win->timer_due = now + win->timer_ticks;
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
// The inbound half of the inversion. In ring 0 the kernel CALLED the
// eleven win_server_ops slots below; a ring-3 compositor is TOLD, one
// WIN_EV_CLIENT_* event per callback, and reads back the detail it
// needs. The handlers are unchanged -- only who invokes them is.
//
// Why the events are thin and this asks for the rest: struct win_event
// is 24 bytes and a title is 32, so carrying the detail inline would
// have meant widening every event in the protocol for the one that
// needs it. The kernel already HAS these facts (it received them), so
// it keeps them and answers WIN_REQ_WINDOW_INFO.


// The window's APP ID -- what its client called itself. Asked once, at
// create: an app id never changes, unlike the title, and it needs its
// own request because WIN_REQ_WINDOW_INFO's single `text` is already the
// title (win_proto.h). Leaves `out` empty rather than failing if the
// window is already gone; an empty id groups with nothing, which is the
// safe direction.
static void query_app_id(int pid, uint32_t id, char *out, unsigned cap,
                          int *out_identity) {
    if (out && cap) out[0] = '\0';
    if (out_identity) *out_identity = -1;
    struct win_request_msg q;
    k_memset(&q, 0, sizeof q);
    q.type = WIN_REQ_WINDOW_APPID;
    q.a = pid;
    q.window = id;
    if (sys_win_request(&q) != 0) return;
    if (out_identity) *out_identity = q.a;
    if (out && cap) {
        unsigned n = 0;
        while (n + 1 < cap && n < WIN_APP_ID_LEN && q.text[n]) { out[n] = q.text[n]; n++; }
        out[n] = '\0';
    }
}

// Maps a client's buffer into this process, so the compositor can read
// its pixels. Idempotent, and must be re-done after a resize: the frames
// are reallocated, and the old mapping is revoked with them.
// --- TWP over a channel ----------------------------------------------
//
// See lib/uwmchan.h. A request that arrives here reached this process
// directly, carrying its payload -- no kernel copy and no read-back.
static struct uchan_server g_chan;

void wm_client_chan_open(void) {
    if (uchan_server_open(&g_chan, WMCHAN_SERVICE) < 0)
        wm_logf("wm: no client channel -- requests take the kernel path\n");
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
    uchan_server_scan(&g_chan);

    int from;
    struct wmchan_msg m;
    while ((from = uchan_server_recv(&g_chan, &m, sizeof m)) != 0) {
        m.text[sizeof m.text - 1] = '\0';
        switch (m.type) {
        // THE PAYLOAD IS HERE, which is the point: each of these took a
        // "something changed" event through the kernel and then a
        // WIN_REQ_WINDOW_INFO to read the detail back, because struct
        // win_event is 24 bytes and none of them fits.
        case WIN_REQ_TITLE:
            on_window_title(from, m.window, m.text);
            break;
        case WIN_REQ_HINTS:
            on_window_hints(from, m.window, (unsigned)m.a, m.b, m.c);
            break;
        case WIN_REQ_CURSOR:
            on_window_cursor(from, m.window, m.a);
            break;
        default:
            // A message this build does not know. Dropped rather than
            // guessed at -- a client speaking a later protocol is not
            // an error the compositor can fix.
            break;
        }
    }
}

// Releases a mapping taken above. THE MAPPING IS WHAT KEEPS A DEAD
// WINDOW'S FRAMES ALIVE, so skipping this leaks them for as long as this
// process runs -- see WIN_REQ_UNMAP_WINDOW.
static void unmap_client_window(int pid, uint32_t id) {
    struct win_request_msg q;
    k_memset(&q, 0, sizeof q);
    q.type = WIN_REQ_UNMAP_WINDOW;
    q.a = pid;
    q.window = id;
    sys_win_request(&q);
}

static uint32_t *map_client_window(int pid, uint32_t id) {
    struct win_request_msg q;
    k_memset(&q, 0, sizeof q);
    q.type = WIN_REQ_MAP_WINDOW;
    q.a = pid;
    q.window = id;
    if (sys_win_request(&q) != 0) return 0;
    // The address is DERIVED, not returned -- see win_proto.h. A fixed
    // per-(pid, window) address is one a test can assert about; one the
    // kernel returned would vary per boot.
    return (uint32_t *)(uintptr_t)win_compositor_vaddr(pid, id);
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
// WIN_REQ_DEBUG_TAKE answers 0 when nothing is pending, which is the
// common case and the whole cost.
void wm_client_poll_debug(void) {
    struct win_debug_msg q;
    k_memset(&q, 0, sizeof q);
    q.type = WIN_REQ_DEBUG_TAKE;
    if (sys_win_debug(&q) != 1) return; // nothing waiting

    static char reply[WIN_DEBUG_REPLY_MAX];
    int n = wm_client_debug_command(q.text, reply, sizeof reply);

    struct win_debug_msg r;
    if (n < 0) {
        // Unrecognised, which a caller must be able to tell from a
        // command that legitimately printed nothing.
        for (unsigned i = 0; i < sizeof r; i++) ((uint8_t *)&r)[i] = 0;
        r.type = WIN_REQ_DEBUG_REPLY;
        r.flags = WIN_DEBUG_F_UNKNOWN;
        r.len = 0;
        sys_win_debug(&r);
        return;
    }

    // Chunked: one message carries WIN_DEBUG_CHUNK bytes and a `gui
    // windows --json` is routinely longer. WIN_DEBUG_F_MORE on every
    // piece but the last, which is what releases the waiting console.
    int sent = 0;
    do {
        int piece = n - sent;
        if (piece > WIN_DEBUG_CHUNK) piece = WIN_DEBUG_CHUNK;
        for (unsigned i = 0; i < sizeof r; i++) ((uint8_t *)&r)[i] = 0;
        r.type = WIN_REQ_DEBUG_REPLY;
        for (int i = 0; i < piece; i++) r.text[i] = reply[sent + i];
        r.text[piece] = '\0';
        r.len = (uint32_t)piece;
        sent += piece;
        if (sent < n) r.flags = WIN_DEBUG_F_MORE;
        sys_win_debug(&r);
    } while (sent < n);
}

int wm_client_handle_event(const struct win_event *ev) {
    if (!ev) return 0;
    int pid = ev->a;
    uint32_t id = ev->window;

    switch (ev->type) {
    case WIN_EV_CLIENT_CREATED: {
        // THE SIZE IS IN THE EVENT, and always was. This used to ask the
        // kernel for it with WIN_REQ_WINDOW_INFO -- a round trip for
        // something it had just been handed -- because the same call
        // fetched the title and the hints, which no longer live there.
        // The title and hints arrive on the channel a moment later; a
        // window is briefly untitled and unconstrained, which is what
        // the placeholder in on_window_created() is for.
        int w = ev->b, h = (int)ev->mods;
        uint32_t *buf = map_client_window(pid, id);
        if (!buf) return 1;
        // The app id came through as "" until WIN_REQ_WINDOW_APPID
        // existed, which left every window on this desktop anonymous --
        // see query_app_id().
        char app_id[WIN_APP_ID_MAX];
        int identity = -1;
        query_app_id(pid, id, app_id, sizeof app_id, &identity);
        // x/y are the compositor's to choose -- the kernel never had an
        // opinion about placement, it only forwarded what the client
        // asked for. 0,0 lets the existing handler place it.
        on_window_created(pid, id, buf, w, h, 0, 0, app_id, identity);
        break;
    }
    case WIN_EV_CLIENT_PRESENT:
        on_window_present(pid, id, (int)ev->b,
                          WIN_PRESENT_W(ev->mods), WIN_PRESENT_H(ev->mods));
        break;
    case WIN_EV_CLIENT_DESTROYED:
        // The pixels are STILL READABLE here: this process's mapping
        // holds a reference to the frames, and the release below is what
        // drops it (win_proto.h, WIN_REQ_UNMAP_WINDOW). The release must
        // come after the handler, which is the last thing that could
        // read them.
        on_window_destroyed(pid, id);
        unmap_client_window(pid, id);
        break;
    // WIN_EV_CLIENT_TITLE / _HINTS / _CURSOR are not delivered any more:
    // those requests reach this process over the channel with their
    // payloads (lib/uwmchan.h), so there is nothing to be told about and
    // nothing to read back.
    case WIN_EV_CLIENT_RESIZED: {
        // Re-map before telling the handler: the frames were
        // reallocated, so the old mapping was revoked with them and the
        // pointer the window list holds is stale.
        uint32_t *buf = map_client_window(pid, id);
        if (!buf) return 1;
        on_window_resized(pid, id, buf, ev->b, (int)ev->mods);
        break;
    }
    case WIN_EV_CLIENT_CURSOR:
        on_window_cursor(pid, id, (int)ev->b);
        break;
    case WIN_EV_CLIENT_PONG:
        on_window_pong(pid, id, (uint32_t)ev->b);
        break;
    case WIN_EV_CLIENT_TIMER:
        on_window_timer(pid, id, (unsigned)ev->b);
        break;
    case WIN_EV_CLIENT_CLOSE:
        on_close_pid(pid);
        break;
    case WIN_EV_CLIENT_ACTIVATE:
        // Told, not asked: the kernel already answered the asking client
        // (it holds the app_ids), so this is only the action.
        on_window_activate_window(pid, id);
        break;
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
        break;
    case WIN_EV_SCREEN:
        wm_screen_changed();
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
    win_events_push(win->client_pid, &ev);
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
    win_events_push(win->client_pid, &ev);
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
              ((unsigned)wm_rawin_mods_now() << WIN_MOUSE_MODS_SHIFT);
    win_events_push(win->client_pid, &ev);
}

// Propose a new CONTENT size. Deliberately a proposal: the WM does not
// resize a client's window, because the buffer belongs to the client
// and only it can decide when that changes. If the client agrees it
// answers with WIN_REQ_RESIZE and on_window_resized() above adopts the
// result; if it ignores this, nothing happens and the window stays as
// it was. Same politeness as the close button.
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
    win_events_push(win->client_pid, &ev);
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
    win_events_push(win->client_pid, &ev);
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
    win_events_push(win->client_pid, &ev);
}

void wm_client_send_close(struct window *win) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_CLOSE;
    ev.window = win->client_win;
    win_events_push(win->client_pid, &ev);

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
    win->ping_serial = g_next_serial;
    win->ping_sent_tick = sys_ticks();

    struct win_event ev = {0};
    ev.type = WIN_EV_PING;
    ev.window = win->client_win;
    ev.a = (int)win->ping_serial;
    win_events_push(win->client_pid, &ev);
}

static void on_window_pong(int pid, uint32_t id, uint32_t serial) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    struct window *w = &windows[idx];
    if (serial != w->ping_serial) return; // stale -- see above
    w->ping_serial = 0;
    if (w->not_responding) {
        w->not_responding = 0;
        redraw_pending = 1; // the title bar said "(Not Responding)"
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
            continue;
        }
        if (now - w->ping_sent_tick < (uint64_t)wm_ping_timeout_ticks) continue;

        if (!w->not_responding) {
            w->not_responding = 1;
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
