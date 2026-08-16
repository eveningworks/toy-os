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
#include "wm_debug.h" // the diagnostic channel's WM end, below
#include "win_server.h"
#include "win_events.h"
#include "kapi.h"
#include "theme.h"

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

static int on_window_created(int pid, uint32_t id, uint32_t *buf,
                              int w, int h, int x, int y) {
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
    win->client_w = w;
    win->client_h = h;
    win->client_last_mx = INT32_MIN; // nothing delivered yet
    win->client_last_my = INT32_MIN;

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
    redraw_pending = 1;
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h); // new taskbar button

    klog_printf("wm: client pid %d opened window %u (%dx%d)\n", pid, id, w, h);
    return 1;
}

static void on_window_present(int pid, uint32_t id) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    // Damage only the CONTENT area, not the whole window: the chrome
    // hasn't changed, and over-damaging is how a compositor quietly
    // stops being a compositor.
    wm_damage_rect(window_content_x(&windows[idx]), window_content_y(&windows[idx]),
                    window_content_w(&windows[idx]), window_content_h(&windows[idx]));
    redraw_pending = 1;
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
    klog_printf("wm: client pid %d closed window %u\n", pid, id);
}

static void on_window_title(int pid, uint32_t id, const char *title) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;

    int i = 0;
    for (; title[i] && i < WIN_TITLE_MAX - 1; i++) windows[idx].title[i] = title[i];
    windows[idx].title[i] = '\0';

    // The title bar only -- see on_window_present() on over-damaging.
    wm_damage_rect(windows[idx].x, windows[idx].y, windows[idx].w, WM_TITLEBAR_H);
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

// The client answered a WIN_EV_RESIZE proposal: its buffer is now this
// size. The WM adopts it here rather than when the proposal was sent,
// which is what stops the chrome and the content ever disagreeing --
// see abi/win_proto.h's configure/ack description.
static void on_window_resized(int pid, uint32_t id, uint32_t *buf, int w, int h) {
    int idx = find_client_window(pid, id);
    if (idx < 0) return;
    struct window *win = &windows[idx];

    // Damage the OLD rect before moving to the new one: shrinking
    // leaves desktop behind that nothing else would repaint.
    wm_damage_rect(win->x, win->y, win->w, win->h);

    win->client_buf = buf;
    win->client_w = w;
    win->client_h = h;
    win->w = w + 2;
    win->h = h + WM_TITLEBAR_H + 2;

    wm_damage_rect(win->x, win->y, win->w, win->h);
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
// it IN PLACE, and what arrives is the transport's message buffer.
static int on_debug_command(const char *line, char *out, int cap) {
    char buf[WIN_DEBUG_CMD_LEN];
    k_strlcpy(buf, line ? line : "", sizeof buf);

    struct dbg_out o = { .buf = out, .cap = cap, .len = 0, .overflow = 0 };
    if (!wm_debug_dispatch_out(buf, &o)) return -1;
    if (o.overflow) dbg_out_write(&o, "gui: (output truncated)\r\n");
    return o.len;
}

static const struct win_server_ops WM_SERVER_OPS = {
    .window_created   = on_window_created,
    .window_present   = on_window_present,
    .window_destroyed = on_window_destroyed,
    .window_title     = on_window_title,
    .window_hints     = on_window_hints,
    .window_resized   = on_window_resized,
    .window_pong      = on_window_pong,
    .debug_command    = on_debug_command,
};

void wm_client_init(void) {
    win_server_register(&WM_SERVER_OPS);
}

void wm_client_shutdown(void) {
    // Unregistered on the way out so a client request made after the
    // desktop has exited is refused, rather than dispatched into a
    // window list that is no longer being drawn.
    win_server_register(0);
}

int wm_client_is_client_window(const struct window *win) {
    return win && win->client_pid != 0 && win->client_buf != 0;
}

void wm_client_draw(const struct window *win) {
    if (!wm_client_is_client_window(win)) return;
    gfx_blit(window_content_x(win), window_content_y(win),
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
    ev.mods = buttons;
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
    win->close_asked_tick = pit_ticks();
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
// WM_PING_TIMEOUT_TICKS is not answering its queue at all. That is
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
    win->ping_sent_tick = pit_ticks();

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
    uint64_t now = pit_ticks();
    int report = -1;

    for (int i = 0; i < window_count; i++) {
        struct window *w = &windows[i];
        if (!wm_client_is_client_window(w)) continue;
        if (!w->ping_serial) continue;
        if (now - w->ping_sent_tick < WM_PING_TIMEOUT_TICKS) continue;

        if (!w->not_responding) {
            w->not_responding = 1;
            redraw_pending = 1;
            klog_printf("wm: client pid %d is not responding\r\n", w->client_pid);
            // Only a window the user has actually asked to close earns a
            // dialog. An app that hangs while nobody is trying to do
            // anything with it gets the title-bar mark and nothing more
            // -- a modal that appears on its own, over whatever the user
            // was doing, for a window they never touched, would be worse
            // than the hang.
            if (w->close_asked_tick) report = i;
        }
    }
    return report;
}
