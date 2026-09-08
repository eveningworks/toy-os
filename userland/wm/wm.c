// The window manager: owns the screen once gui_main() hands off to it.
// Keeps a small fixed array of windows in z-order (index 0 = back,
// highest index = front/focused) and drives everything through a single
// event loop -- mouse clicks, dragging, keyboard routed to the focused
// window, and a full-screen redraw whenever something changes.
//
// This file holds the shared state, the app-facing helpers (wm.h's
// window_* functions), window lifecycle (open/close/focus), and
// wm_run()'s main loop -- the "core" a reader should start at. Mouse/
// keyboard handling is wm_input.c; all drawing is wm_render.c. See
// wm_internal.h for the private glue between the three -- none of this
// split changes behavior, it's the same single-threaded event loop as
// before, just organized into files by concern.
//
// Rendering draws the whole scene every time a window/taskbar/menu
// actually changes (see wm_render.c) rather than tracking per-window
// dirty rectangles -- much simpler to get right with overlapping movable
// windows, at the cost of being slower than a "real" compositor. It draws
// into an off-screen buffer and flips the finished frame in one pass
// (gfx_set_double_buffered / gfx_present), so the repaint isn't visible
// as flicker. gfx_present() itself only blits the sub-rectangle that
// actually changed (see its own comment in gfx.c) rather than the whole
// screen, which is what makes the other common case -- the mouse moving
// with nothing else changing -- cheap without needing real dirty-rect
// tracking of the scene: see wm_render_cursor_move() in wm_render.c.
#include "wm_internal.h"
#include "wm_geometry.h"
#include "wm_debug.h"
#include "win_server.h"
#include "win_events.h"
#include "start_menu.h"
#include "context_menu.h"
#include "calendar_popup.h"
#include "volume_popup.h"
#include "brightness_popup.h"
#include "wm_overlay.h"
#include "osk.h"
#include "confirm_dialog.h"
#include "file_picker.h"
#include "desktop.h"
#include "wm_tray.h"
#include "wm_taskbar.h"
#include "cursor_theme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_log.h"
#include "wm/wm_rawin.h"

struct window *windows = NULL;
int window_count = 0;
static int windows_cap = 0;

// Grows the window table by doubling. See wm_internal.h for the two
// rules the caller owes (index, don't cache a `struct window *`).
//
// sbrk + copy, and the old block is LEAKED rather than freed -- ring 3
// has no free (SYS_SBRK only grows), which is the one place this port is
// genuinely worse than the ring-0 original. It is bounded and small: the
// table doubles, so reaching N windows leaks under N entries in total, a
// few KiB at any window count a person will reach. A real allocator
// (Milestone 24) turns this back into a free(); until then the tradeoff
// is written down rather than hidden behind a wrapper that looks like
// malloc and silently never releases.
int wm_windows_reserve(int n) {
    if (n <= windows_cap) return 1;

    int cap = windows_cap ? windows_cap : WM_WINDOWS_INITIAL;
    while (cap < n) cap *= 2;

    struct window *grown = sys_sbrk((int64_t)((size_t)cap * sizeof *grown));
    if (grown == (void *)-1) grown = 0;
    if (!grown) {
        // The only way a window can be refused now. Logged rather than
        // silent: the old fixed-table refusal did nothing at all and
        // read as a dead Start menu.
        sys_eprint("wm: out of memory growing the window table\n");
        return 0;
    }

    k_memset(grown, 0, (size_t)cap * sizeof *grown);
    if (windows) {
        k_memcpy(grown, windows, (size_t)window_count * sizeof *grown);
        // No free -- see above.
    }
    windows = grown;
    windows_cap = cap;
    return 1;
}

int screen_w, screen_h;
int taskbar_h;

// Title-bar minimize/maximize/close button state -- see
// wm_internal.h's comment on title_btn_armed_win for the full
// press-hover-commit-on-release story.
int title_btn_armed_win = -1;
int title_btn_armed_kind = -1;
int title_btn_pressed_active = 0;
int title_hover_win = -1;
int title_hover_kind = -1;

int wm_exit_requested = 0;

int dragging = -1; // index into windows[], or -1 if not dragging
int drag_off_x, drag_off_y;

int resizing = -1; // index into windows[], or -1 if not resizing
int resize_right = 0, resize_bottom = 0;
int resize_start_mx, resize_start_my;
int resize_start_w, resize_start_h;

// THE INTERACTIVE RESIZE, at most one at a time. A client's window
// follows the pointer like any other (as it does on Windows and in
// KDE), but it does so by ASKING: `want` is the content size the drag
// implies, `sent` is the last size actually proposed, and only one
// proposal is with the client at a time so a slow app sets the pace.
// See wm_input.c's resize_pump().
int resize_ask_idx = -1;
int resize_ask_pid = 0;
unsigned resize_ask_win = 0;
int resize_want_w = -1, resize_want_h = -1;
int resize_sent_w = -1, resize_sent_h = -1;
int resize_inflight = 0;
uint64_t resize_sent_tick = 0;

// THE OUTLINE A DRAG IS SHOWING, or window index -1 when the drag is
// moving the real window (which is the default for both). Screen
// coordinates, frame size -- wm_render.c draws exactly this rect and
// decides nothing.
int drag_outline_win = -1;
int drag_outline_x, drag_outline_y, drag_outline_w, drag_outline_h;

// What THIS drag is showing, resolved once when it starts from
// `desktop.move_mode` / `desktop.resize_mode`. `resize_auto` is the
// third resize choice still watching: live until the client misses a
// deadline, an outline after that (see wm_input.c's resize_pump()).
int move_outline_mode = 0;
int resize_outline_mode = 0;
int resize_auto = 0;

// The last ask-to-shown latency, in MILLISECONDS: how long the window
// took to become a size it was asked for. `auto` compares this against
// its threshold; `gui state` reports it, because the number is the
// whole basis of that decision and nothing else can see it.
unsigned resize_lag_ms = 0;
// Proposals sent, ever. Monotonic, and reported by `gui state`,
// because whether a resize is INTERACTIVE is a count rather than
// anything a screenshot can show: a WM that only asked on release
// would move this by exactly one per drag.
unsigned resize_asks = 0;

int content_dragging = -1; // index into windows[], or -1 -- see wm_internal.h
int content_pressed = -1; // index into windows[], or -1 -- see wm_internal.h
int content_pressed_btn = 1; // which button armed it -- see wm_internal.h

int redraw_pending = 1;

// ---- app-facing helpers (declared in wm.h) ----

void window_set_state(struct window *win, void *state) { win->app_state = state; }
void *window_get_state(struct window *win) { return win->app_state; }

int window_content_x(const struct window *win) { return win->x + 1; }
int window_content_y(const struct window *win) { return win->y + WM_TITLEBAR_H + 1; }
int window_content_w(const struct window *win) { return win->w - 2; }
int window_content_h(const struct window *win) { return win->h - WM_TITLEBAR_H - 2; }

void window_invalidate(struct window *win) {
    wm_damage_rect(win->x, win->y, win->w, win->h);
    redraw_pending = 1;
}

// --- read-only window introspection (see wm.h's declarations) ---

int wm_window_count(void) { return window_count; }

const struct window *wm_get_window(int index) {
    if (index < 0 || index >= window_count) return 0;
    return &windows[index];
}

// Which icon a WINDOW should show. A window knows the app_id its client
// declared; a .desktop entry knows which app_id it launches (its AppId=
// key, freedesktop's StartupWMClass). Matching the two is what lets a
// window be given artwork the WM did not launch it with.
//
// NULL for a window whose client declared no app id, or whose app id
// matches no entry -- a stand-alone test client, say. Those keep the
// plain text they have always had, in the taskbar and in the title bar.
//
// It lived in wm_taskbar.c while the taskbar was the only asker. It is
// here now because the title bar asks the same question (see
// title_icon() in wm_render.c), and "which app is this window" is a
// property of the window, not of the strip that happens to draw it.
const char *wm_window_icon_name(int idx) {
    if (idx < 0 || idx >= window_count) return 0;
    const char *id = windows[idx].app_id;
    if (!id || !id[0]) return 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        const struct gui_app *a = &gui_app_registry[i];
        if (a->app_id && k_strcmp(a->app_id, id) == 0) return a->icon_name;
    }
    return 0;
}

// ---- window lifecycle ----

void bring_to_front(int idx) {
    if (idx == window_count - 1) return;

    // A real reorder -- this window's own footprint is now drawn on
    // top of whatever it overlaps instead of wherever it was in
    // z-order before, so it needs repainting even though its geometry
    // isn't changing (compute_window_damage()'s geometry diff,
    // wm_render.c, wouldn't notice this on its own). This alone is
    // sufficient for the floating windows' OCCLUSION: bring_to_front()
    // only ever promotes ONE window to the front, shifting others down
    // a slot without changing their relative order to each OTHER, so
    // no other window's stacking relative to another changes. But the
    // previously-frontmost window's own titlebar tint (focused blue vs.
    // unfocused gray, draw_window_chrome()'s `focused` param) DOES
    // change even though its geometry doesn't -- it needs its own rect
    // damaged too, not just the newly-promoted window's. (Was missed
    // when this file's damage reporting first shipped -- harmless back
    // then since every window's chrome still got *drawn*, just clipped
    // away outside the damage rect; became a real visible bug once
    // wm_render.c's Phase 3 started skipping the draw call entirely for
    // undamaged windows -- see docs/decisions.md.) The taskbar strip is
    // a separate matter too -- draw_taskbar()'s per-button tint depends
    // on which window is frontmost, so both the newly- and
    // previously-frontmost buttons need a repaint.
    struct window *prev_front = &windows[window_count - 1];
    wm_damage_rect(prev_front->x, prev_front->y, prev_front->w, prev_front->h);
    wm_damage_rect(windows[idx].x, windows[idx].y, windows[idx].w, windows[idx].h);
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);

    // Tell the clients involved, BEFORE the reorder -- prev_front is
    // still the front window here, and windows[idx] is still the one
    // being promoted. A client cannot work focus out for itself: it
    // sees keys only while focused, and "no keys" looks the same as
    // "the user is thinking".
    if (prev_front != &windows[idx]) {
        wm_client_send_focus(prev_front, 0);
        wm_client_send_focus(&windows[idx], 1);
    }

    struct window tmp = windows[idx];
    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
    windows[window_count - 1] = tmp;

    // Reordering used to need three more fixups here, keeping
    // pending_write_win/pending_read_win/pending_proc_win pointing at
    // the same window across the shuffle. R9 deleted those slots, so the
    // reorder is now just the reorder.
}

static int find_window_for_app(const struct gui_app *app) {
    for (int i = 0; i < window_count; i++)
        if (windows[i].app == app) return i;
    return -1;
}

void open_app(const struct gui_app *app) {
    // A launcher entry (gui_apps.h's `exec_path`) is a ring-3 PROGRAM,
    // not a kernel-space app: spawn it and get out of the way. It
    // builds its own window through the windowing protocol, which
    // wm_client.c turns into a real one once the process gets that far,
    // so there is nothing to create here and nothing to wait for.
    //
    // Note this does NOT go through window_start_process(): that slot
    // exists to route an exit back to a specific window's
    // on_process_exit callback, and it is WM-global (one at a time).
    // A launched client has no such callback, and its window is torn
    // down by the window server when the process dies -- so tracking it
    // here would cap the desktop at one ring-3 app for no benefit.
    if (app->exec_path) {
        int pid = sys_spawn(app->exec_path, 0, -1);
        wm_logf("wm: launched %s (%s) as pid %d\n",
                     app->name, app->exec_path, pid);
        if (pid > 0) wm_track_launched(pid);
        // pid 0 means no free process slot, or the binary is missing
        // from /bin. Say so in the log rather than failing silently --
        // from the desktop the only symptom is a menu item that does
        // nothing, which is indistinguishable from a missed click.
        if (pid == 0) {
            wm_logf("wm: launch FAILED -- no process slot, or no %s\n",
                         app->exec_path);
        }
        return;
    }

    // Single-instance apps (the default -- see gui_apps.h's
    // `multi_instance` flag): reopening from the Start menu just
    // focuses/restores the one window that can ever exist, same as
    // always. Multi-instance apps (Calculator) skip this check
    // entirely and always fall through to opening a brand new window
    // -- MAX_WINDOWS is still the only cap on how many.
    if (!app->multi_instance) {
        int existing = find_window_for_app(app);
        if (existing >= 0) {
            if (windows[existing].state == WIN_MINIMIZED) windows[existing].state = WIN_NORMAL;
            bring_to_front(existing);
            redraw_pending = 1;
            return;
        }
    }
    // Grow rather than refuse. This used to be
    // `if (window_count >= MAX_WINDOWS) return;` -- a SILENT no-op, so
    // clicking a seventh app in the Start menu did nothing at all with
    // no message anywhere.
    if (!wm_windows_reserve(window_count + 1)) return;

    struct window *win = &windows[window_count];
    k_memset(win, 0, sizeof(*win));

    int cascade = (window_count % 5) * 24;
    win->x = 60 + cascade;
    win->y = 40 + cascade;
    // The window that is ABOUT to stop being frontmost repaints its
    // title bar (focused blue -> unfocused grey), so it has to be
    // damaged too -- exactly what bring_to_front() below already does
    // for the same reason. Opening a window used to damage only the new
    // one, leaving the old title bar showing the focused colour until
    // something else happened to repaint it.
    //
    // Found by `gui damage verify on` (wm_render.c), seconds after that
    // mode first ran: "4350 px changed outside the damage rect, first at
    // (61,41)" -- (61,41) being the previously-focused window's title
    // bar. Nothing else had noticed it.
    if (window_count > 0) {
        struct window *losing_focus = &windows[window_count - 1];
        wm_damage_rect(losing_focus->x, losing_focus->y,
                        losing_focus->w, losing_focus->h);
        // ...and if it is a CLIENT, it has to be TOLD, not just
        // repainted: a client sees nothing but its own event queue, so
        // an unannounced focus loss leaves it drawing a caret for input
        // that is going somewhere else. wm_client.c's own create path
        // has always done this; opening a kernel-space app over a client
        // did not, which is a path nothing exercised until a test
        // stopped opening its second window first.
        wm_client_send_focus(losing_focus, 0);
    }

    int content_w, content_h;
    app->default_size(&content_w, &content_h);
    win->w = content_w + 2;
    win->h = content_h + WM_TITLEBAR_H + 2;
    win->state = WIN_NORMAL;
    win->app = app;
    win->resizable = app && app->resizable;
    win->open = 1;

    int i = 0;
    for (; app->name[i] && i < WIN_TITLE_MAX - 1; i++) win->title[i] = app->name[i];
    win->title[i] = '\0';

    window_count++;
    // AFTER window_count++, because restoring takes an INDEX: it calls
    // wm_ensure_reachable(), which addresses windows by index and would
    // not see a slot the count does not cover yet.
    wm_geometry_restore(window_count - 1);
    if (app->on_open) app->on_open(win);
    redraw_pending = 1;
    // A new taskbar button appears -- compute_window_damage()
    // (wm_render.c) will damage the new window's own rect (its
    // last_w == 0 sentinel), but the taskbar strip is a separate area.
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);

    sys_eprint("wm: opened ");
    sys_eprint(app->name);
    sys_eprint("\n");
}

// Pids this desktop launched, so their slots can be reaped.
//
// A process that exits stays SCHED_ZOMBIE until somebody polls it
// (scheduler.h). The Terminal's children are reaped by the ring-3
// shell's waitpid; a Start-menu launch has no shell, so nothing was
// polling these at all and every open-then-close of a launched app
// burned a slot permanently. With MAX_PROCS at 4, as it was then, that
// was four launches per boot, after which the desktop silently opened
// nothing -- measured, not deduced: the fifth `gui open Shapes` simply
// never produced a window. (MAX_PROCS is 64 now, which raises the
// ceiling without removing the need to reap.)
//
// Reaping is also what makes force-quit repeatable rather than a
// four-shot escape hatch.
// Sized by SCHED_MAX_PROCS, not by the window count. It tracks
// PROCESSES the desktop launched, so the scheduler's table is its
// real bound -- the two were the same number by coincidence while
// windows were capped at 6, and that coincidence broke the moment
// the window table started growing.
static int g_launched[SCHED_MAX_PROCS];

// Slot i's pid, or 0 if free. For `gui state`, which is how a test sees
// that a process the desktop launched is still alive -- the WM reaps
// this table every iteration, so a non-zero entry means "running as of
// the last loop pass" without the test needing a second liveness call.
int wm_launched_pid(int slot) {
    if (slot < 0 || slot >= SCHED_MAX_PROCS) return 0;
    return g_launched[slot];
}

int wm_launched_max(void) { return SCHED_MAX_PROCS; }

void wm_track_launched(int pid) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (g_launched[i] == 0) { g_launched[i] = pid; return; }
    }
    // Full: every slot is a pid still running. Dropping the newest is
    // the honest outcome -- it just means this one is not reaped until
    // the next boot, which is where we started.
    sys_eprint("wm: launch table full -- this pid will not be reaped\n");
}

static void wm_reap_launched(void) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (!g_launched[i]) continue;
        int code = 0;
        // sys_waitpid_NOHANG(), and that distinction is the whole
        // desktop. The blocking sys_waitpid() PARKS until the child
        // exits, so reaping a client that is merely RUNNING stopped the
        // WM dead on its first window -- no crash, no fault, no log --
        // and the comment that used to sit here asserted it was
        // "non-blocking in the same sense scheduler_poll() was". It was
        // not, and SYS_WAITPID's own first ABI line said "BLOCKS".
        //
        // SYS_RETRY means "still running" here rather than "ask again",
        // which is what the flag buys; looping on it would restore the
        // bug exactly.
        if (sys_waitpid_nohang(g_launched[i], &code) != SYS_RETRY) {
            g_launched[i] = 0; // reaped (or already gone) -- slot is free again
        }
    }
}

// --- force quit -------------------------------------------------------
//
// The window a force-quit dialog is currently about. An index would go
// stale the moment any other window closed (close_window() reshuffles
// windows[], which has bitten pending_write/read/proc for exactly this
// reason), and the dialog outlives several frames, so this holds the
// pid instead -- which nothing reshuffles.
static int g_force_quit_pid;

static void wm_force_quit_yes(void) {
    if (!g_force_quit_pid) return;
    wm_logf("wm: force-quitting pid %d\r\n", g_force_quit_pid);

    // Killing the process is what takes the window down: scheduler_kill
    // calls win_server_client_gone(), which destroys the client's
    // windows through the same path a normal exit uses. Removing the
    // window here as well would be a second teardown of the same thing.
    // SIGKILL: a client that has stopped answering its event queue is
    // exactly the case a catchable signal cannot reach.
    sys_kill(g_force_quit_pid, SIGKILL);
    g_force_quit_pid = 0;
    redraw_pending = 1;
}

static void wm_force_quit_no(void) {
    // "Wait" restarts the clock rather than giving up: an app that was
    // merely slow gets another chance, and a wedged one is offered again
    // the next time the user asks it to close.
    //
    // CLEARING close_asked_tick IS WHAT RE-ARMS THAT -- the offer is now
    // once per ASK (force_quit_offered_tick), so a later Alt+F4 stamps a
    // new tick and qualifies again. Clearing not_responding beside it is
    // cosmetic: the periodic ping re-flags a still-wedged window within
    // interval + timeout, which is honest, since it really is still hung.
    for (int i = 0; i < window_count; i++) {
        if (windows[i].client_pid == g_force_quit_pid) {
            windows[i].close_asked_tick = 0;
            windows[i].force_quit_offered_tick = 0;
            windows[i].ping_serial = 0;
            windows[i].not_responding = 0;
            break;
        }
    }
    redraw_pending = 1; // the title bar drops "(Not Responding)" for now
    g_force_quit_pid = 0;
    redraw_pending = 1;
}

static void wm_offer_force_quit(int idx) {
    if (confirm_dialog_open) return;   // already asking about something
    if (idx < 0 || idx >= window_count) return;

    static char msg[WIN_TITLE_MAX + 32];
    k_snprintf(msg, sizeof msg, "%s is not responding.", windows[idx].title);

    g_force_quit_pid = windows[idx].client_pid;
    confirm_dialog_open_labelled(msg, "Force Quit", "Wait",
                                  wm_force_quit_yes, wm_force_quit_no);
}

// See wm_internal.h. The reasoning lives in wm_input.c's X-button
// comment, which this was extracted from -- a client's window is the
// CLIENT's to close, because it may have unsaved state and because the
// process would otherwise go on drawing into a buffer that is no longer
// on screen.
void wm_request_close(int idx) {
    if (idx < 0 || idx >= window_count) return;
    if (wm_client_is_client_window(&windows[idx])) {
        wm_client_send_close(&windows[idx]);
        return;
    }
    close_window(idx); // shifts windows[] -- no caller may touch idx again
}

void close_window(int idx) {
    // Refuse to close a window with a write in flight -- pending_write's
    // handle is polled by index (pending_write_win), and the callback it
    // eventually fires (gui_apps.h's on_write_complete) is delivered to
    // &windows[pending_write_win]; closing mid-write would either shift
    // that slot to point at a DIFFERENT window by the time the write
    // finishes, or (if this is the last window) leave it dangling. Same
    // "block while pending" choice as window_start_write() refusing a
    // second concurrent write. See wm.h's window_write_pending().
    // Give the app a chance to release whatever it allocated for this
    // window (multi-instance apps kmalloc/kzalloc their own per-window
    // state -- see gui_apps.h's `multi_instance` flag and
    // apps/calculator.c) BEFORE the slot shifts out of existence.
    // Single-instance apps generally leave this NULL -- their state is
    // a static struct with nothing to free.
    if (windows[idx].app && windows[idx].app->on_close) {
        windows[idx].app->on_close(&windows[idx]);
    }

    // WHERE THIS WINDOW WAS, for the next launch. Here rather than at
    // the five places geometry actually changes: a drag rewrites x/y
    // every frame, so saving there would be hundreds of whole-file
    // rewrites per drag. This is the one chokepoint that sees the final
    // answer, and it still has the live slot -- the array shift at the
    // bottom of this function is what would lose it.
    wm_geometry_save(&windows[idx]);

    // If the FRONT window is going away, whatever ends up frontmost
    // gains keyboard focus -- and a client has to be told, since it
    // sees keys only while focused. Sent before the shift, while the
    // indices still mean what they say.
    if (idx == window_count - 1 && window_count > 1) {
        wm_client_send_focus(&windows[window_count - 2], 1);
    }

    // Report the closing window's own rect as damage BEFORE the array
    // shift below removes it -- whatever's now revealed there (desktop
    // or another window) needs repainting. Skipped if it was minimized
    // (not drawn there in the first place, so nothing to reveal). The
    // taskbar strip always needs it too, regardless of minimized state
    // -- its button disappears and every later button shifts left.
    if (windows[idx].state != WIN_MINIMIZED) {
        wm_damage_rect(windows[idx].x, windows[idx].y, windows[idx].w, windows[idx].h);
    }
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);

    // Closing the FRONTMOST window hands focus to the one below it, and
    // that window's title bar changes (focused blue vs. unfocused gray,
    // draw_window_chrome()'s `focused` param) without its geometry
    // changing at all -- so compute_window_damage() (wm_render.c) can't
    // notice it, and the rect damaged above only covers it where the
    // two happened to overlap. This is bring_to_front()'s prev_front gap
    // seen from the other end, and it has the same consequence now that
    // wm_render.c's Phase 3 skips an undamaged window's chrome entirely:
    // the inheriting window keeps drawing as unfocused until something
    // else repaints it. Found by `gui damage verify on` (via
    // tools/damage_sweep.py): "11038 px changed outside the damage rect,
    // first at (109,89)" on closing the front window, (109,89) being the
    // title bar of the one underneath.
    //
    // Only the last slot matters -- closing any other window shifts
    // slots down without changing which one is frontmost. A minimized
    // inheritor isn't drawn, so it needs nothing beyond the taskbar
    // strip already damaged above.
    if (idx == window_count - 1 && window_count >= 2) {
        struct window *inheritor = &windows[window_count - 2];
        if (inheritor->state != WIN_MINIMIZED) {
            wm_damage_rect(inheritor->x, inheritor->y, inheritor->w, inheritor->h);
        }
    }

    sys_eprint("wm: closed ");
    sys_eprint(windows[idx].app ? windows[idx].app->name : windows[idx].title);
    sys_eprint("\n");

    // A DRAG IN PROGRESS NAMES A WINDOW BY INDEX, and the shift below
    // makes that index somebody else. Cancel it here rather than let a
    // release move or resize the wrong window; an outline on screen is
    // dropped with it.
    if (dragging == idx || resizing == idx || drag_outline_win == idx) {
        if (drag_outline_win >= 0) {
            wm_damage_rect(drag_outline_x, drag_outline_y,
                           drag_outline_w, drag_outline_h);
        }
        drag_outline_win = -1;
        if (dragging == idx) dragging = -1;
        if (resizing == idx) resizing = -1;
    }

    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
    window_count--;
    redraw_pending = 1;

}


// compositor_raw() is GONE (M41 stage 4c, R9).
//
// It pushed every raw input the WM had just polled to the REGISTERED
// compositor, so a second consumer could exist alongside the ring-0
// desktop -- stage 2's whole point, and what `compositor_test.py`
// proves by asserting each injected event twice. In ring 3 the WM IS
// the registered compositor, so that call forwarded input to itself.
// Input now arrives the other way round: see wm_rawin.c.

// ---- live reload of /usr/wm/desktop ----
//
// Drop a .desktop file in and it appears, the way KDE and Explorer watch
// their desktop folders. There is no inotify here, so the mechanism is
// sys_fs_generation() (api/fs.h): a counter the VFS bumps on every change to
// the filesystem.
//
// **The idle cost is one integer compare per frame and no I/O at all.**
// That is the entire reason the counter exists rather than this
// re-listing the directory on a timer -- a timer would mean a real disk
// read every few seconds forever on a machine doing nothing, which is
// the opposite of subtle.
//
// The counter is global, so any filesystem change wakes this, not just
// one in /usr/wm/desktop. A Notepad save costs one small directory read
// and finds nothing changed. That is the deliberate trade: per-path
// watches would need a registry, a lifetime and an eviction policy to
// save a read that only happens when something already changed.
static void poll_desktop_entries(void) {
    static uint64_t seen_gen;
    static uint64_t quiet_until;
    static int primed;

    // First call adopts the current value rather than treating the whole
    // boot as a change -- the registry was just loaded from this very
    // directory.
    if (!primed) {
        primed = 1;
        seen_gen = sys_fs_generation();
        return;
    }

    uint64_t gen = sys_fs_generation();
    if (gen == seen_gen) return; // the common case, and it is free

    // Never mid-interaction. The selection, the armed click and the drag
    // are all REGISTRY INDICES, and a reload renumbers them; an open
    // Start menu would have its rows move under the cursor between press
    // and release. Deferring costs nothing -- the generation stays
    // changed, so this fires as soon as the interaction ends.
    if (start_menu_open || desktop_drag_active()) return;

    // And never while a window still POINTS INTO the registry.
    // struct window::app is a pointer into gui_app_registry[] (wm.h),
    // and gui_apps_load() rewrites that array in place -- re-parsing
    // every entry and re-sorting the structs. Reloading underneath an
    // open window would silently rebind it to whatever entry landed in
    // that slot, so its callbacks would belong to a different app.
    //
    // Only a kernel-space app's window holds one; a ring-3 client's is 0
    // (wm_client.c), which is every window a user is likely to have open
    // today. So this defers rather than disables, and it disappears
    // entirely with the last builtin in M41's stage 4.
    for (int i = 0; i < window_count; i++) {
        if (windows[i].app) return;
    }

    uint64_t now = sys_ticks();
    if (now < quiet_until) return;
    quiet_until = now + 50; // ~500ms at the PIT's 100Hz

    seen_gen = gen;

    // sys_fs_generation() is GLOBAL: it moves for a write anywhere, and
    // almost every one of them is not a .desktop file. Ask the cheap
    // question first -- one directory listing, no file reads -- so
    // saving a setting stops costing a full re-read of every entry.
    // That re-read froze the desktop for 2.5s under KVM.
    static uint64_t seen_fp;
    static int fp_primed;
    uint64_t fp = gui_apps_dir_fingerprint();
    if (fp_primed && fp == seen_fp) return;
    fp_primed = 1;
    seen_fp = fp;

    // Timed in two halves because they fail differently: gui_apps_load()
    // re-reads and re-parses every .desktop file (disk), while
    // desktop_entries_changed() rebuilds the icon layout (no I/O of its
    // own). A reload that takes hundreds of milliseconds is worth a line
    // naming WHICH -- the watchdog can only see the phase as a whole.
    uint64_t t0 = sys_ticks();
    gui_apps_load();
    uint64_t t1 = sys_ticks();
    desktop_entries_changed();
    uint64_t t2 = sys_ticks();
    if ((t2 - t0) * 10 >= 100) {
        wm_logf("wm: desktop reload took %u ms (parse %u ms, layout %u ms)\n",
                    (uint32_t)((t2 - t0) * 10), (uint32_t)((t1 - t0) * 10),
                    (uint32_t)((t2 - t1) * 10));
    }
    wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
    redraw_pending = 1;
}

// ---- main loop ----

// WIN_EV_SCREEN. Unlike the font, the screen's size IS cached in
// places: the grant and the back buffer, the pointer's seed, the icon
// grid's rows, every window's position, and a maximized window's size.
// Each is re-derived here; the overlays are simply closed, since they
// re-clamp on their next open, and the last thing is an unconditional
// frame.
void wm_screen_changed(void) {
    if (!ugfx_screen_remode(&g_wm_screen)) {
        wm_logf("wm: screen changed but the grant could not be re-mapped\n");
        return;
    }
    screen_w = g_wm_screen.back.w;
    screen_h = g_wm_screen.back.h;
    wm_rawin_clamp(screen_w, screen_h);
    wm_layout_changed();
    wm_hwcursor_invalidate();
    wm_logf("wm: screen changed -- %dx%d, %d scanout(s)\n", screen_w, screen_h,
            g_wm_screen.buffers);
}

// The usable area moved -- a new screen size, or a new taskbar height
// (wm_taskbar.c). Everything derived from `screen_h - taskbar_h` is
// re-derived here: the icon grid's rows, each maximized window's size,
// every other window's clamp, and the overlays, which are simply closed
// since they re-clamp on their next open.
void wm_layout_changed(void) {
    desktop_entries_changed();   // the icon grid's rows depend on the height

    wm_overlay_close_others(0);

    for (int i = 0; i < window_count; i++) {
        struct window *w = &windows[i];
        int is_client = w->client_pid > 0;
        if (w->state == WIN_MAXIMIZED) {
            w->x = 0; w->y = 0;
            if (is_client)
                wm_client_send_resize(w, screen_w - 2, screen_h - taskbar_h - WM_TITLEBAR_H - 2);
            else { w->w = screen_w; w->h = screen_h - taskbar_h; }
            continue;
        }
        // The drag clamp's rule: keep a grip's worth of the title bar on
        // screen and the whole bar clear of the taskbar.
        int keep = ugfx_char_w() * 8;
        if (keep > w->w) keep = w->w;
        if (w->x > screen_w - keep) w->x = screen_w - keep;
        if (w->x < keep - w->w) w->x = keep - w->w;
        int max_y = screen_h - taskbar_h - WM_TITLEBAR_H;
        if (w->y > max_y) w->y = max_y;
        if (w->y < 0) w->y = 0;
    }
    wm_render_reset();
    redraw_pending = 1;
}

void wm_run(void) {
    // Claim the compositor role, then take the framebuffer grant it
    // gates. Both can be refused -- another process may already hold the
    // role, and the grant is refused to anyone who does not -- so this
    // returns rather than faulting, and says which half failed.
    //
    // There is no vga_write() counterpart here any more: the text
    // console belongs to the kernel, which restores it when the
    // compositor goes away (R7). A ring-3 WM that cannot start says so
    // on stderr and exits; the kernel is what puts the user back at a
    // shell.
    if (!wm_claim_compositor()) {
        sys_eprint("wm: cannot enter GUI mode -- compositor role refused\n");
        return;
    }
    if (!ugfx_screen_init(&g_wm_screen)) {
        sys_eprint("wm: cannot enter GUI mode -- no framebuffer grant "
                   "(not the compositor, unsupported pixel format, or no "
                   "room for a back buffer)\n");
        return;
    }

    // Allocate the window table up front so `windows` is never NULL
    // while the desktop is up. Every loop over it is bounded by
    // window_count and so is already safe at zero, but a desktop that
    // cannot hold one window is not a desktop -- fail here, where there
    // is somewhere to say so, rather than at the first Start-menu click.
    if (!wm_windows_reserve(WM_WINDOWS_INITIAL)) {
        sys_eprint("wm: out of memory for the window table\n");
        return;
    }

    // THE FONT. A ring-3 process has no glyph tables of its own -- they
    // are ~11,800 lines of kernel .rodata, mapped READ-ONLY on request
    // (WIN_REQ_FONT) so a client's text cannot drift from the desktop's.
    // Every window client gets this inside uapp_run(); the WM is not a
    // uapp, so it has to ask for itself.
    //
    // Without it ugfx_char_h() is 0, and the failure is not "no text" --
    // it is that plus every font-derived measurement collapsing:
    // WM_TITLEBAR_H is `ugfx_char_h() + 8`, so chrome becomes 8px, the
    // taskbar becomes a sliver, and icon labels vanish while their boxes
    // still draw. That exact picture is what the first person to run
    // `gui3` saw.
    if (!ugfx_font_init()) {
        sys_eprint("wm: cannot enter GUI mode -- the server refused the font\n");
        return;
    }

    screen_w = g_wm_screen.back.w;
    screen_h = g_wm_screen.back.h;
    // The configured height, if any, arrives on the first
    // taskbar_poll_config() before the first frame.
    taskbar_h = taskbar_default_h();

    // Registers the two cursor settings and loads the configured theme.
    // Before the first frame, so the pointer is themed from the moment
    // it is first drawn rather than snapping a frame later.
    cursor_theme_init();

    // Seed the pointer now that the screen's size is known -- this is
    // what mouse_init() used to do by resetting the device to centre.
    wm_rawin_init(screen_w, screen_h);

    wm_logf("wm: entering GUI mode (%dx%d)\n", screen_w, screen_h);

    // No gfx_set_double_buffered() here, and there cannot be one: a
    // compositor's back buffer is not a MODE it can turn off. It is the
    // surface it owns, allocated by ugfx_screen_init() above, and
    // drawing straight at the granted framebuffer instead is not a
    // degraded fallback but a correctness error -- that mapping is
    // write-combining, where every anti-aliased glyph's read-back is an
    // uncached round trip. If the buffer could not be allocated,
    // ugfx_screen_init() already failed and we returned.
    //
    // Mouse bounds and mouse_init() are gone with it. The pointer's
    // position arrives as WIN_EV_RAW_MOUSE, already in screen
    // coordinates and already bounded by whoever owns the device -- a
    // ring-3 compositor does not initialise hardware.

    // Start accepting client windows. Registered here rather than at
    // boot so a ring-3 client that runs outside GUI mode is refused
    // (SYS_WIN_REQUEST returns -1) instead of drawing into a buffer
    // nothing will ever composite -- see wm_client.c.
    wm_client_init();
    // AFTER the compositor role is claimed, so a client that finds the
    // beacon has a compositor able to serve it.
    wm_client_chan_open();

    // Build the app list from /usr/wm/desktop/ before anything draws a
    // menu or an icon. Data on disk, not a compiled-in table -- see
    // apps/gui_apps.c.
    gui_apps_load();

    window_count = 0;
    wm_overlay_close_others(0);
    dragging = -1;
    resizing = -1;
    content_dragging = -1;
    content_pressed = -1;
    content_pressed_btn = 1;
    title_btn_armed_win = -1;
    title_btn_armed_kind = -1;
    title_btn_pressed_active = 0;
    title_hover_win = -1;
    title_hover_kind = -1;
    redraw_pending = 1;
    wm_exit_requested = 0;

    wm_render_reset(); // first frame must be a full repaint -- see wm_render.c
    tray_init();
    osk_init();   // its tray item, beside the clock's
    volume_tray_init();   // the tray's second item, after the clock takes slot 0
    brightness_tray_init(); // the third

    // Announced once per run of this loop. Reset here rather than
    // declared static-and-forgotten, because `gui` can re-enter it:
    // announcing again after an "Exit to shell" and a restart is
    // correct, and the kernel treats a repeat announcement as the same
    // announcement anyway.
    int announced_ready = 0;

    int mx, my;
    uint8_t buttons;
    wm_rawin_mouse(&mx, &my, &buttons);
    int prev_mx = mx, prev_my = my;
    uint8_t prev_buttons = buttons;

    for (;;) {
        // The ring-0 loop halted here until the next interrupt. `hlt` is
        // PRIVILEGED, so in ring 3 it is a #GP -- and it was the first
        // thing this desktop hit on its very first run.
        //
        // What replaces it is a WAIT WITH A DEADLINE, because a
        // compositor has both kinds of wake source at once: input and
        // client requests arrive as EVENTS, while the clock, the pings
        // and its clients' timers are work nobody sends it an event for.
        // Waiting on only the events stops the clock; waiting on only a
        // deadline is the poll this used to be. `poll()` with a timeout
        // is how every Wayland compositor and the X server spell it.
        //
        // What it bought is smaller than it sounds -- 42% of a host core
        // to 37% over an idle 30 s -- because the kernel's idle context
        // spins rather than halting (docs/roadmap.md).
        //
        // READINESS, not delivery: sys_wait_ready() consumes nothing, so
        // wm_rawin_pump() below still drains the whole queue. A wait that
        // returned an event would hide one from the pump every time.
        //
        // TWO THINGS MUST DEFEAT THE WAIT, and both are invisible to the
        // kernel. Injected input from the debug console lives in ring-3
        // memory, so pushing to it wakes nobody -- and the loop takes one
        // per iteration, so a press would block with its release still
        // queued. And a repaint we already owe should not wait 100 ms to
        // happen, which also keeps the first frame prompt.
        {
            uint32_t wait_ms = WM_IDLE_WAIT_MS;
            uint64_t due = wm_client_next_timer_due();
            if (due) {
                uint64_t now_t = sys_ticks();
                uint32_t in_ms = (due > now_t) ? (uint32_t)((due - now_t) * 10) : 0;
                if (in_ms < wait_ms) wait_ms = in_ms;
            }
            if (redraw_pending || wm_debug_work_pending()) wait_ms = 0;

            // Through the channel when there is one, so a client's
            // message defeats this park exactly as a kernel event does
            // -- one wait over both, which is what the wakeword is for.
            if (wm_client_chan_ready()) wm_client_chan_wait((int)wait_ms);
            else sys_wait_ready(wait_ms);
        }

        // Everything from here to wmwd_frame_end() is this frame's WORK.
        // The WAIT above is deliberately outside it: time spent parked
        // is time the WM was not asked to do anything, and counting it
        // would turn every idle frame into a slow one -- and, worse,
        // would stop a silent watchdog from meaning "the stall was not
        // ours". See wm_watchdog.c.
        wmwd_frame_begin();

        // Picks up a cursor theme or size changed from Control Panel or
        // by editing /etc/toyos.conf. One generation compare per frame
        // and no I/O unless it moved -- the same trick the `.desktop`
        // reload above uses.
        wmwd_phase("cursor_theme");
        cursor_theme_poll();

        // The kernel's idle work is NOT called from here any more, and
        // this is the deletion `scheduler_idle()` was created to make
        // possible. In ring 0 this loop was what kept the serial debug
        // console answering while the desktop was up -- and every GUI
        // test tool arrives over that console, so moving the WM out
        // would have taken the whole test harness with it. Naming the
        // work kernel-side first (M41 stage 4a, R5) meant the migration
        // deletes a CALL rather than the capability: the kernel drains
        // the console from its own idle path, whoever is running.
        wmwd_phase("idle");

        // One integer compare unless the filesystem actually changed.
        wmwd_phase("desktop_entries");
        poll_desktop_entries();

        // The wallpaper, on the same generation counter. Its own
        // watchdog phase because it is the one that can be SLOW: a
        // decode is ~180 ms for a 1280x720 JPEG under TCG, and a slow
        // frame blamed on 'desktop_entries' would send the next reader
        // to the wrong file.
        wmwd_phase("wallpaper");
        desktop_poll_config();

        // The Start button's appearance, on the same counter. Its own
        // phase for the watchdog's sake but not because it is slow --
        // unchanged it is one compare, and a change costs one icon
        // decode that the cache then keeps.
        wmwd_phase("startbutton");
        taskbar_poll_config();
        calendar_poll_config(); // `desktop.week_start`, same generation poll
        volume_poll_config();   // the level and the device list, and the debounced write
        brightness_poll_config();

        // Drain everything the kernel has queued for us, then read the
        // position out of it. One pump per frame, fully draining -- see
        // wm_rawin.c on why partial draining backs up.
        wmwd_phase("input");
        wm_rawin_pump();
        // AFTER the event queue, never before: a window is created
        // through the kernel and announced there, while its title
        // arrives on the channel. See lib/uwmchan.h.
        wm_client_chan_pump();
        wm_rawin_mouse(&mx, &my, &buttons);


        // The `gui` command channel. Polled, not event-driven -- see
        // wm_client_poll_debug(). Every GUI test tool arrives here.
        wm_client_poll_debug();

        // Synthetic input from the serial debug console's `gui click` /
        // `gui drag` (apps/wm/wm_debug.c), consumed at most one event
        // per iteration so each becomes its own tick -- which is what
        // puts a press and its release on separate frames, as every
        // arm-on-press/commit-on-release control here requires.
        //
        // Overrides the real mouse for this iteration only; it does not
        // move the actual cursor, so once the queue drains the pointer
        // is wherever the user left it. Fine for the scripted-test use
        // this exists for, and worth knowing before reading a `gui
        // state` cursor position mid-sequence.
        {
            int ix, iy;
            uint8_t ib;
            if (wm_debug_next_input(&ix, &iy, &ib)) {
                mx = ix; my = iy; buttons = ib;
            }
        }
        int mouse_moved = (mx != prev_mx || my != prev_my);

        // Only on a CHANGE. This loop runs on every timer tick, so an
        // unconditional push would overflow a 32-deep queue in a
        // fraction of a second and report constant drops while the user
        // sat still. See WIN_EV_RAW_MOUSE.
        int left_edge_down = (buttons & 0x1) && !(prev_buttons & 0x1);
        if (left_edge_down) wm_handle_left_click(mx, my);

        // bit1 = right button (see mouse.h's mouse_get_state()) -- same
        // edge-triggered pattern as the left button above, dispatching
        // to context_menu.h's popup instead of a window action.
        int right_edge_down = (buttons & 0x2) && !(prev_buttons & 0x2);
        if (right_edge_down) wm_handle_right_click(mx, my);

        // A Start-menu action (currently just "Exit to shell") may have
        // just set this -- bail out the same way Esc used to, before
        // touching drag/resize state for a click that was never about a
        // window in the first place. Deferred while a write is pending
        // (below still runs, so the write keeps stepping to completion
        // -- wm_exit_requested stays set and this fires on the very next
        // tick after it finishes) rather than abandoning it mid-write:
        // pending_write's handle owns kernel heap state (see TFS2's
        // struct tfs_write_step) that only gets freed on a terminal
        // fs_write_range_step() result. Same reasoning for pending_read
        // (struct tfs_read_step), and for pending_proc: whichever window
        // started it (apps/terminal.c) has an active vga_sink installed
        // (vga.h) pointing at ITS scrollback -- exiting GUI mode with
        // that still installed would silently swallow the physical
        // shell's own prompt output the moment control returned to it
        // (see vga.h's struct vga_sink doc comment on why a stale sink
        // is dangerous), not just abandon the process.
        if (wm_exit_requested) {
            sys_eprint("wm: exiting GUI mode, returning to shell\n");
            // Stop accepting client windows before the desktop stops
            // drawing them -- same reasoning as the pending_* guards
            // above, one layer down: a request serviced after this
            // point would attach a window to a list nobody repaints.
            wm_client_shutdown();
            // Hand the screen back to the console, which owns its own
            // double buffering (see vga.h's vga_present()) rather than
            // inheriting whatever the WM left the flag set to. This used
            // to turn buffering OFF, which left the console drawing --
            // and, when it scrolled, READING -- the framebuffer directly.
            // No vga_resume(). The text console belongs to the kernel,
            // which restores it when it sees the compositor deregister --
            // cleanly, by a kill, or by a fault, all one path (R7). A
            // ring-3 WM resuming it would be reaching for a device it
            // does not own.
            return;
        }

        // Per-frame poll for a pending process (Milestone 1 phase 4b,
        // docs/roadmap.md) -- same pending_proc_win accuracy guarantee
        // from bring_to_front()/close_window() as the two blocks above.
        // Unlike those, this poll isn't what makes the process's own
        // output appear -- that already streams straight into the
        // owning window's scrollback via vga_putc()'s active sink
        // (vga.h) the instant each SYS_WRITE syscall runs, driven by
        // scheduler_tick() on every timer interrupt regardless of
        // whether wm_run() happens to be looping right now. This poll
        // only detects completion (SCHED_POLL_EXITED) so the window can
        // be told, and redraws unconditionally while a process is
        // pending so output that streamed in between two mouse-move
        // events still shows up promptly instead of waiting for some
        // unrelated redraw to happen to fire.
        wmwd_phase("clients");
        wm_reap_launched();

        // Client timers, once per frame. This is what a client blocks
        // on instead of polling -- see WIN_REQ_TIMER.
        wm_client_check_timers();

        // Liveness, once per frame. Only reports a window that has gone
        // unresponsive WHILE being asked to close -- see
        // wm_client_check_liveness().
        {
            int hung = wm_client_check_liveness();
            if (hung >= 0) wm_offer_force_quit(hung);
        }

        // Mouse movement alone takes wm_render_cursor_move()'s cheap
        // path below (cursor sprite only, not a full scene repaint).
        //
        // THE START MENU USED TO OPT OUT OF THAT ENTIRELY: its hover
        // highlight was derived from (mx, my) inside the draw, so every
        // move forced a full repaint for as long as the menu was open --
        // measured at 60 ms a move on a 1280x720 TCG guest, and 20 of 20
        // moves logged as slow frames against 0 of 20 with the menu
        // closed. It tracks its hovered row now and damages only its own
        // rect when that row changes, so a move within one row costs
        // nothing at all.
        //
        // The calendar still forces one -- it derives its `<`/`>` hover
        // the old way, and doing to it what was done to the menu is the
        // same three steps (docs/roadmap.md).
        // EVERY OVERLAY'S HOVER, from one table (wm_overlay.h). This
        // used to name three of the six popups here and two more forty
        // lines down, each with its own idea of when to damage -- and
        // whichever one was forgotten had an invisible hover rather
        // than a slow one, because a move alone takes the cursor-only
        // path below and never repaints the scene.
        // AND THE REPAINT IS ASKED FOR, not merely damaged. wm_damage_rect()
        // records a rectangle; it does not schedule a frame, and a plain
        // move takes the cursor-only path below -- so a hover change that
        // only damaged was invisible until something ELSE repainted, which
        // on an idle desktop is the tray clock, once a second. That is the
        // "laggy highlight" symptom exactly, and e960ad3 introduced it by
        // moving each overlay's hover into one table and dropping the
        // `redraw_pending = 1` the per-overlay code had carried -- while
        // fixing the same symptom on the volume flyout.
        if (mouse_moved && wm_overlay_hover(mx, my, buttons)) redraw_pending = 1;

        // Closes the Start menu once a just-clicked row's brief flash
        // has shown long enough -- independent of clicks/movement, so
        // it still fires even if the mouse hasn't moved since the
        // click. See start_menu.c's own comment on this.
        start_menu_update();

        // Title-bar button hover/press feedback -- hover only matters
        // when the mouse actually moved (same cheap-path reasoning as
        // everything else in this loop); the press-commit check runs
        // every tick regardless, same as content_pressed below, since
        // it also needs to react to the button being released even if
        // the mouse hasn't moved since.
        if (mouse_moved) wm_update_title_hover(mx, my);
        wm_update_title_btn_press(mx, my, buttons);

        // Modal overlays track their own press/hover the same way, and
        // for the same reason: their buttons arm on press and commit on
        // release, so they need the live cursor every tick, not just the
        // button-down edge wm_handle_left_click() sees. Both are no-ops
        // while their dialog is closed.
        // Live press tracking for whichever overlays have a draggable
        // control -- a slider, a button that arms on press. They need
        // the cursor every tick, not just the button-down edge
        // wm_handle_left_click() sees. The hover half of what used to
        // be here moved into wm_overlay_hover() above, which applies
        // the "nothing re-hovers under a held button" rule for all six
        // rather than for these two.
        wm_overlay_press(mx, my, buttons);

        // Content hover, on the same only-when-the-mouse-moved cheap
        // path as the title-bar hover above. It also has to run once
        // after a button is RELEASED (the suppression inside it lifts
        // then, and the control under the cursor should light up again
        // without needing a further wiggle) -- hence the `|| !buttons`.
        if (mouse_moved || !(buttons & 0x1)) wm_update_content_hover(mx, my, buttons);

        wm_update_drag_resize(mx, my, buttons);
        desktop_update_drag(mx, my, buttons); // desktop icon drag, if one's in progress -- see desktop.h

        static uint64_t last_second = (uint64_t)-1;
        uint64_t ticks = sys_ticks();
        uint64_t this_second = ticks / 100;
        if (this_second != last_second) {
            last_second = this_second;
            tray_update_clock(); // also sets redraw_pending + damages the taskbar strip
            volume_tray_update(); // the speaker icon follows the level
        }

        // Esc used to always exit the window manager here -- replaced by
        // the Start menu's "Exit to shell" (see wm_system_actions above),
        // which is discoverable instead of a hidden key. Esc itself is
        // deliberately unclaimed at the WM level now, free for a future
        // per-window or modal use (e.g. canceling a confirm dialog)
        // instead of double-booking it as "exit everything".
        uint8_t key_mods = 0;
        int key_down = 1;
        int key = wm_rawin_take_key(&key_mods, &key_down);
        // A `gui key` from the debug console, if the real keyboard had
        // nothing -- deliberately second, so a human at the keyboard is
        // never pre-empted by a queued test keystroke.
        if (key == -1) {
            int injected = wm_debug_next_key_mods(&key_mods);
            if (injected) key = injected;
        }

        int wheel = wm_rawin_take_wheel();
        if (wheel == 0) wheel = wm_debug_next_wheel(); // `gui wheel`, same
                                                        // second-place rule as
                                                        // the injected key above

        // THE TRAY TAKES THE WHEEL FIRST, and only over its own item or
        // its open panel -- which is where KDE, GNOME and Windows all
        // put volume-by-wheel. Anywhere else it falls through to the
        // focused window, so a scrollable app is unaffected.
        if (wheel != 0 && volume_handle_wheel(mx, my, wheel)) wheel = 0;
        if (wheel != 0 && brightness_handle_wheel(mx, my, wheel)) wheel = 0;

        if (key != -1 || wheel != 0) {
            // A modal file picker (e.g. Notepad's Save As...) captures
            // keyboard input first, same "most modal" priority
            // wm_handle_left_click() already gives confirm_dialog/
            // file_picker over ordinary window clicks -- see
            // file_picker.h. It has no wheel handling yet, so a wheel
            // event while it's open is just dropped rather than
            // reaching the window behind it.
            if (key != -1 && file_picker_handle_key(key)) {
                // consumed by the picker -- modal, its own rect isn't
                // reported as damage yet, falls back to a full-screen
                // repaint same as other dialogs (see wm_render.c's
                // damage-region comment)
            } else {
                int f = -1;
                for (int i = window_count - 1; i >= 0; i--) {
                    if (windows[i].state != WIN_MINIMIZED) { f = i; break; }
                }

                // Alt+F4 closes the focused window, and is handled HERE
                // rather than delivered to the app -- a window-manager
                // shortcut, exactly as it is in Windows (routed through
                // DefWindowProc to WM_SYSCOMMAND/SC_CLOSE) and in KDE
                // (a KWin global shortcut). The app still decides what
                // happens, because this asks through the same
                // wm_request_close() the X button uses and a client may
                // refuse it; what the app does NOT get is the chance to
                // silently swallow the keystroke, which is the whole
                // point of the shortcut existing.
                //
                // Matched on the modifier bit rather than a dedicated
                // KEY_ALT_F4 code: nothing is folded for a function key
                // the way Ctrl/Alt are folded into a letter, so mods are
                // usable here, and this generalises to any future
                // Alt+F<n> without a new code each time.
                // Super/Win TOGGLES the Start menu -- open if closed,
                // close if open, exactly as on Windows and KDE. A
                // window-manager shortcut like Alt+F4 below, consumed
                // here so it never reaches the focused window: a
                // full-screen app must not be able to swallow the Start
                // menu.
                //
                // Suppressed while a MODAL overlay owns input (the
                // confirm dialog, the file picker). Those take the
                // screen deliberately, and opening a menu behind one
                // would leave two things claiming the next click.
                // The context menu is not modal in that sense and is
                // simply replaced.
                if (key != -1 && !key_down) {
                    // A RELEASE GOES STRAIGHT TO THE FOCUSED CLIENT, and
                    // takes none of the shortcut branches below: Super
                    // and Alt+F4 act on the PRESS, exactly as
                    // docs/gui-guidelines.md's arm-then-commit rule says
                    // a control should, and firing them again on the way
                    // up would toggle the Start menu twice per keystroke.
                    //
                    // A client can therefore see a release whose press
                    // the WM consumed -- Super held over a window, say.
                    // It has to tolerate that, and every real system says
                    // the same: an X11 grab produces exactly this shape.
                    // Tracking held keys means ignoring an up you have no
                    // down for, which is the sane implementation anyway.
                    if (f >= 0 && !file_picker_open &&
                        wm_client_is_client_window(&windows[f])) {
                        wm_client_send_key_up(&windows[f], key, key_mods);
                    }
                } else if (key == KEY_SUPER) {
                    if (!confirm_dialog_open && !file_picker_open) {
                        // Toggle; opening closes every other popup
                        // through the overlay table (wm_overlay.h).
                        if (start_menu_open) start_menu_close();
                        else start_menu_open_now();
                        redraw_pending = 1;
                    }
                } else if (key == KEY_F4 && (key_mods & KEY_MOD_ALT) && f >= 0 && !file_picker_open) {
                    wm_request_close(f); // may shift windows[] -- f is dead after this
                    redraw_pending = 1;
                } else if (f >= 0 && key != -1 && !file_picker_open && wm_client_is_client_window(&windows[f])) {
                    // Focused window belongs to a ring-3 client: the
                    // key becomes a protocol message rather than a
                    // callback. Same focus rule either way -- who gets
                    // the key is the WM's decision, and it doesn't
                    // change because the recipient is a process.
                    wm_client_send_key(&windows[f], key, key_mods);
                    redraw_pending = 1;
                } else if (f >= 0 && key != -1 && !file_picker_open && windows[f].app && windows[f].app->on_key) {
                    windows[f].app->on_key(&windows[f], key, key_mods);
                }
                if (f >= 0 && wheel != 0 && !file_picker_open) {
                    // A client gets the same notches as a message. Until
                    // TWP carried a wheel event, only kernel-space apps
                    // could scroll -- so Notepad drew a scrollbar it
                    // could never move.
                    if (wm_client_is_client_window(&windows[f])) {
                        wm_client_send_wheel(&windows[f], wheel);
                    } else if (windows[f].app && windows[f].app->on_wheel) {
                        windows[f].app->on_wheel(&windows[f], wheel);
                    }
                }
                // Typing/scrolling is the single most common redraw
                // trigger this loop sees besides mouse movement -- worth
                // reporting precisely rather than falling back to a full
                // repaint for every keystroke.
                if (f >= 0) wm_damage_rect(windows[f].x, windows[f].y, windows[f].w, windows[f].h);
            }
            redraw_pending = 1;
        }

        prev_mx = mx; prev_my = my; prev_buttons = buttons;

        // With double buffering there's no flicker to hide, so redraw as
        // soon as anything changes -- throttling here only added cursor lag.
        //
        // Two tiers: anything that actually changed the scene (a click,
        // a drag/resize in progress, a window opening/closing, a key or
        // wheel event delivered to an app, the once-a-second clock tick)
        // sets redraw_pending and gets a full repaint. Mouse movement
        // alone -- by far the most common event this loop sees -- takes
        // wm_render_cursor_move()'s cheap path instead: it doesn't touch
        // redraw_pending at all, so a plain full redraw still happens
        // exactly when it used to. See wm_render.c's dirty-rectangle
        // comments for why this split is worth having.
        wmwd_phase("render");
        int rendered = 0;
        if (redraw_pending) {
            redraw_pending = 0;
            wm_render_frame(mx, my);
            rendered = 1;
        } else if (mouse_moved || wm_cursor_shape_changed(mx, my)) {
            // The shape moves while the mouse does not: a client answers
            // a motion event frames later, by which time the pointer has
            // usually stopped.
            wm_render_cursor_move(mx, my);
        }

        // THE DESKTOP IS USABLE AT ITS FIRST COMPOSITED FRAME, and that
        // is why the announcement is here and not at the compositor
        // claim four hundred lines up. Everything between the two can
        // still fail -- the framebuffer grant, the font, the cursor
        // theme, the desktop entries -- and a desktop that has claimed
        // the role but drawn nothing is exactly the state `After=` used
        // to be unable to tell from a working one.
        //
        // sys_notify_ready() is a no-op for anything init does not
        // supervise, so this is correct whether the desktop was started
        // as a service or by hand from the shell. See rt/sys.h.
        if (rendered && !announced_ready) {
            announced_ready = 1;
            sys_notify_ready();
        }

        wmwd_frame_end();
    }
}
