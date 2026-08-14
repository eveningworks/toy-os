// Client windows, presentation side: the window manager acting as the
// window SERVER for ring-3 processes.
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
    if (window_count >= MAX_WINDOWS) return 0; // no room -- the create fails

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

static const struct win_server_ops WM_SERVER_OPS = {
    .window_created   = on_window_created,
    .window_present   = on_window_present,
    .window_destroyed = on_window_destroyed,
    .window_title     = on_window_title,
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

void wm_client_send_close(struct window *win) {
    if (!wm_client_is_client_window(win)) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_CLOSE;
    ev.window = win->client_win;
    win_events_push(win->client_pid, &ev);
}
