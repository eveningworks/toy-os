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
#include "wm_debug.h"
#include "win_server.h"
#include "win_events.h"
#include "start_menu.h"
#include "context_menu.h"
#include "confirm_dialog.h"
#include "file_picker.h"
#include "desktop.h"
#include "wm_tray.h"
#include "cursor_theme.h"
#include "kapi.h"
#include "demo.h"

struct window *windows = NULL;
int window_count = 0;
static int windows_cap = 0;

// Grows the window table by doubling. See wm_internal.h for the two
// rules the caller owes (index, don't cache a `struct window *`).
//
// kmalloc + copy + kfree rather than a realloc: this kernel's heap has
// no realloc, and at a few hundred bytes per window the copy is
// irrelevant next to opening a window at all.
int wm_windows_reserve(int n) {
    if (n <= windows_cap) return 1;

    int cap = windows_cap ? windows_cap : WM_WINDOWS_INITIAL;
    while (cap < n) cap *= 2;

    struct window *grown = kmalloc((size_t)cap * sizeof *grown);
    if (!grown) {
        // The only way a window can be refused now. Logged rather than
        // silent: the old fixed-table refusal did nothing at all and
        // read as a dead Start menu.
        klog_write("wm: out of memory growing the window table\n");
        return 0;
    }

    k_memset(grown, 0, (size_t)cap * sizeof *grown);
    if (windows) {
        k_memcpy(grown, windows, (size_t)window_count * sizeof *grown);
        kfree(windows);
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

// A CLIENT window's resize is a PROPOSAL, not a live change: the buffer
// belongs to the client, so the WM tracks the size the drag implies and
// draws an outline, rather than growing a frame around pixels that are
// still the old size. Sent as WIN_EV_RESIZE on release. -1 = no
// proposal in flight, which is also what an app window's resize leaves
// it at (those still resize live -- the WM owns their pixels).
int resize_prop_w = -1, resize_prop_h = -1;

int content_dragging = -1; // index into windows[], or -1 -- see wm_internal.h
int content_pressed = -1; // index into windows[], or -1 -- see wm_internal.h

void *pending_write = 0; // handle from fs_write_range_begin(), or NULL -- see wm_internal.h
int pending_write_win = -1; // index into windows[] the write belongs to, or -1

void *pending_read = 0; // handle from fs_read_range_begin(), or NULL -- see wm_internal.h
int pending_read_win = -1; // index into windows[] the read belongs to, or -1

int pending_proc = 0; // pid from scheduler_spawn(), or 0 -- see wm_internal.h
int pending_proc_win = -1; // index into windows[] the process belongs to, or -1

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

int window_write_pending(void) { return pending_write != 0; }

int window_start_write(struct window *win, void *write_handle) {
    if (pending_write) return 0; // a write's already in flight -- WM-global single slot, see wm_internal.h
    pending_write = write_handle;
    pending_write_win = -1;
    for (int i = 0; i < window_count; i++) {
        if (&windows[i] == win) { pending_write_win = i; break; }
    }
    return 1;
}

int window_read_pending(void) { return pending_read != 0; }

int window_start_read(struct window *win, void *read_handle) {
    if (pending_read) return 0; // a read's already in flight -- WM-global single slot, see wm_internal.h
    pending_read = read_handle;
    pending_read_win = -1;
    for (int i = 0; i < window_count; i++) {
        if (&windows[i] == win) { pending_read_win = i; break; }
    }
    return 1;
}

int window_process_pending(void) { return pending_proc != 0; }

int window_start_process(struct window *win, int pid) {
    if (pending_proc) return 0; // a process's already in flight -- WM-global single slot, see wm_internal.h
    pending_proc = pid;
    pending_proc_win = -1;
    for (int i = 0; i < window_count; i++) {
        if (&windows[i] == win) { pending_proc_win = i; break; }
    }
    return 1;
}

// --- read-only window introspection (see wm.h's declarations) ---

int wm_window_count(void) { return window_count; }

const struct window *wm_get_window(int index) {
    if (index < 0 || index >= window_count) return 0;
    return &windows[index];
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

    // Reordering shifts every slot between idx and the old last slot
    // down by one, and moves idx's own window to the end -- keep
    // pending_write_win pointing at the same window (see close_window()
    // for why this has to stay accurate: the QMP test this phase ships
    // with explicitly clicks a DIFFERENT window mid-save, which lands
    // exactly here).
    if (pending_write_win == idx) pending_write_win = window_count - 1;
    else if (pending_write_win > idx) pending_write_win--;

    // Same bookkeeping for a pending read -- see wm_internal.h's
    // pending_read.
    if (pending_read_win == idx) pending_read_win = window_count - 1;
    else if (pending_read_win > idx) pending_read_win--;

    // Same bookkeeping for a pending process -- see wm_internal.h's
    // pending_proc.
    if (pending_proc_win == idx) pending_proc_win = window_count - 1;
    else if (pending_proc_win > idx) pending_proc_win--;
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
        int pid = scheduler_spawn(app->exec_path, 0);
        klog_printf("wm: launched %s (%s) as pid %d\n",
                     app->name, app->exec_path, pid);
        if (pid > 0) wm_track_launched(pid);
        // pid 0 means no free process slot, or the binary is missing
        // from /bin. Say so in the log rather than failing silently --
        // from the desktop the only symptom is a menu item that does
        // nothing, which is indistinguishable from a missed click.
        if (pid == 0) {
            klog_printf("wm: launch FAILED -- no process slot, or no %s\n",
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
    if (app->on_open) app->on_open(win);
    redraw_pending = 1;
    // A new taskbar button appears -- compute_window_damage()
    // (wm_render.c) will damage the new window's own rect (its
    // last_w == 0 sentinel), but the taskbar strip is a separate area.
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);

    klog_write("wm: opened ");
    klog_write(app->name);
    klog_write("\n");
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
    klog_write("wm: launch table full -- this pid will not be reaped\n");
}

static void wm_reap_launched(void) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (!g_launched[i]) continue;
        int code = 0;
        if (scheduler_poll(g_launched[i], &code) != SCHED_POLL_RUNNING) {
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
    klog_printf("wm: force-quitting pid %d\r\n", g_force_quit_pid);

    // Killing the process is what takes the window down: scheduler_kill
    // calls win_server_client_gone(), which destroys the client's
    // windows through the same path a normal exit uses. Removing the
    // window here as well would be a second teardown of the same thing.
    scheduler_kill(g_force_quit_pid, -1);
    g_force_quit_pid = 0;
    redraw_pending = 1;
}

static void wm_force_quit_no(void) {
    // "Wait" restarts the clock rather than giving up on the window: an
    // app that was merely slow gets another chance, and one that is
    // truly wedged will offer the dialog again next time the user asks
    // it to close. Not re-arming would make the first Wait permanent.
    for (int i = 0; i < window_count; i++) {
        if (windows[i].client_pid == g_force_quit_pid) {
            windows[i].close_asked_tick = 0;
            windows[i].ping_serial = 0;
            // not_responding TOO. check_liveness() reports only the
            // TRANSITION into that state, so leaving the flag set made
            // the first Wait permanent: the window stayed marked, the
            // transition never happened again, and no later Alt+F4
            // could ever re-offer the dialog. Measured, not reasoned --
            // the second Alt+F4 in a row simply did nothing.
            windows[i].not_responding = 0;
            break;
        }
    }
    redraw_pending = 1; // the title bar drops "(Not Responding)"
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
    if (idx == pending_write_win) return;

    // Same refusal for a read in flight -- see wm.h's window_read_pending().
    if (idx == pending_read_win) return;

    // Same refusal for a process in flight -- see wm.h's window_process_pending().
    if (idx == pending_proc_win) return;

    // Give the app a chance to release whatever it allocated for this
    // window (multi-instance apps kmalloc/kzalloc their own per-window
    // state -- see gui_apps.h's `multi_instance` flag and
    // apps/calculator.c) BEFORE the slot shifts out of existence.
    // Single-instance apps generally leave this NULL -- their state is
    // a static struct with nothing to free.
    if (windows[idx].app && windows[idx].app->on_close) {
        windows[idx].app->on_close(&windows[idx]);
    }

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

    klog_write("wm: closed ");
    klog_write(windows[idx].app ? windows[idx].app->name : windows[idx].title);
    klog_write("\n");

    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
    window_count--;
    redraw_pending = 1;

    // A different window closing shifts every later slot down by one --
    // keep pending_write_win pointing at the same window if it moved.
    if (pending_write_win > idx) pending_write_win--;
    if (pending_read_win > idx) pending_read_win--;
    if (pending_proc_win > idx) pending_proc_win--;
}

// ---- raw input to a registered compositor (M41 stage 2) ----
//
// A SECOND consumer of the same input stream this loop reads, running
// alongside the real routing rather than replacing it. Stage 4 deletes
// the routing below and leaves this; until then both are live, which is
// what makes the eventual flip a deletion instead of a cutover.
//
// **The tap has to be here, not at the driver.** `gui click` and
// `gui key` inject through wm_debug.c and are applied AFTER the real
// driver read (see the two override blocks in the loop). Tapping
// mouse_get_state() directly would make every synthetic event invisible
// to the compositor -- i.e. invisible to the 13 GUI test tools, which
// are the only proof any of this works.
//
// A no-op with no compositor registered, which is every ordinary boot.
static void compositor_raw(uint32_t type, int32_t a, int32_t b, uint32_t mods) {
    int pid = win_server_compositor_pid();
    if (!pid) return;

    struct win_event ev = {
        .type = type,
        .window = 0, // meaningless for raw input -- no window chosen yet
        .a = a,
        .b = b,
        .mods = mods,
        .reserved = 0,
    };
    win_events_push(pid, &ev);
}

// ---- live reload of /usr/wm/desktop ----
//
// Drop a .desktop file in and it appears, the way KDE and Explorer watch
// their desktop folders. There is no inotify here, so the mechanism is
// fs_generation() (api/fs.h): a counter the VFS bumps on every change to
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
        seen_gen = fs_generation();
        return;
    }

    uint64_t gen = fs_generation();
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

    uint64_t now = pit_ticks();
    if (now < quiet_until) return;
    quiet_until = now + 50; // ~500ms at the PIT's 100Hz

    seen_gen = gen;
    gui_apps_load();
    desktop_entries_changed();
    wm_damage_rect(0, 0, screen_w, screen_h - taskbar_h);
    redraw_pending = 1;
}

// ---- main loop ----

void wm_run(void) {
    if (!gfx_init()) {
        vga_write("gui: no linear RGB framebuffer available from GRUB\n");
        klog_write("wm: cannot enter GUI mode -- no linear RGB framebuffer from GRUB\n");
        return;
    }

    // Allocate the window table up front so `windows` is never NULL
    // while the desktop is up. Every loop over it is bounded by
    // window_count and so is already safe at zero, but a desktop that
    // cannot hold one window is not a desktop -- fail here, where there
    // is somewhere to say so, rather than at the first Start-menu click.
    if (!wm_windows_reserve(WM_WINDOWS_INITIAL)) {
        vga_write("gui: out of memory for the window table\n");
        return;
    }

    screen_w = gfx_width();
    screen_h = gfx_height();
    taskbar_h = WM_TITLEBAR_H;

    // Registers the two cursor settings and loads the configured theme.
    // Before the first frame, so the pointer is themed from the moment
    // it is first drawn rather than snapping a frame later.
    cursor_theme_init();

    klog_write("wm: entering GUI mode (");
    klog_write_dec((uint32_t)screen_w);
    klog_write("x");
    klog_write_dec((uint32_t)screen_h);
    klog_write(")\n");

    // Draw off-screen and flip completed frames -- without this the
    // full-screen repaint below is visible as flicker while it happens.
    // If the mode is too large to buffer we just draw directly; it still
    // works, it just flickers (see gfx_set_double_buffered).
    //
    // SAY SO when it fails. The consequence is not only flicker: drawing
    // direct means every anti-aliased glyph reads the pixel under it
    // back, and on real hardware that read comes from uncached MMIO, so
    // the desktop goes from slow to unusable. A mode one pixel past
    // GFX_MAX_PIXELS used to take that path in complete silence.
    if (!gfx_set_double_buffered(1)) {
        klog_write("wm: mode too large to double-buffer -- drawing direct "
                    "(expect flicker, and very slow text on real hardware)\n");
    }

    mouse_set_bounds(screen_w, screen_h);
    mouse_init();

    // Start accepting client windows. Registered here rather than at
    // boot so a ring-3 client that runs outside GUI mode is refused
    // (SYS_WIN_REQUEST returns -1) instead of drawing into a buffer
    // nothing will ever composite -- see wm_client.c.
    wm_client_init();

    // Build the app list from /usr/wm/desktop/ before anything draws a
    // menu or an icon. Data on disk, not a compiled-in table -- see
    // apps/gui_apps.c.
    gui_apps_load();

    window_count = 0;
    start_menu_open = 0;
    context_menu_close();
    dragging = -1;
    resizing = -1;
    content_dragging = -1;
    content_pressed = -1;
    title_btn_armed_win = -1;
    title_btn_armed_kind = -1;
    title_btn_pressed_active = 0;
    title_hover_win = -1;
    title_hover_kind = -1;
    redraw_pending = 1;
    wm_exit_requested = 0;
    pending_write = 0;
    pending_write_win = -1;
    pending_read = 0;
    pending_read_win = -1;
    pending_proc = 0;
    pending_proc_win = -1;

    wm_render_reset(); // first frame must be a full repaint -- see wm_render.c
    tray_init();

    int mx, my;
    uint8_t buttons;
    mouse_get_state(&mx, &my, &buttons);
    int prev_mx = mx, prev_my = my;
    uint8_t prev_buttons = buttons;

    for (;;) {
        __asm__ volatile ("hlt");

        // Everything from here to wmwd_frame_end() is this frame's WORK.
        // The `hlt` above is deliberately outside it: time spent halted
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

        // The kernel's idle work, which it owns rather than this loop
        // (scheduler.h). What it does today is keep the serial debug
        // console answering while the desktop is up -- and every GUI
        // test tool arrives over that console, so when this loop becomes
        // a ring-3 process (Milestone 41 stage 4) this line is DELETED
        // and the capability stays. That is the whole reason it moved.
        wmwd_phase("idle");
        scheduler_idle();

        // One scripted step per iteration, when a demo is running (see
        // apps/demo.h). A no-op on every ordinary boot -- `demo` has to
        // be on the kernel command line for a script to have been loaded
        // at all.
        wmwd_phase("demo");
        demo_gui_tick();

        // One integer compare unless the filesystem actually changed.
        wmwd_phase("desktop_entries");
        poll_desktop_entries();

        wmwd_phase("input");
        mouse_get_state(&mx, &my, &buttons);

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
        if (mouse_moved || buttons != prev_buttons) {
            compositor_raw(WIN_EV_RAW_MOUSE, mx, my, buttons);
        }

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
        // pending_write's handle owns kernel heap state (see tfs.c's
        // struct tfs_write_step) that only gets freed on a terminal
        // fs_write_range_step() result. Same reasoning for pending_read
        // (struct tfs_read_step), and for pending_proc: whichever window
        // started it (apps/terminal.c) has an active vga_sink installed
        // (vga.h) pointing at ITS scrollback -- exiting GUI mode with
        // that still installed would silently swallow the physical
        // shell's own prompt output the moment control returned to it
        // (see vga.h's struct vga_sink doc comment on why a stale sink
        // is dangerous), not just abandon the process.
        if (wm_exit_requested && !pending_write && !pending_read && !pending_proc) {
            klog_write("wm: exiting GUI mode, returning to shell\n");
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
            vga_resume();
            return;
        }

        // Poll one step of a pending write, once per frame, instead of
        // wm_run() ever calling fs_write_range() and blocking (Milestone
        // 1 phase 3, docs/roadmap.md) -- Notepad's Save is the first
        // caller (see wm.h's window_start_write()). One step is one
        // filesystem block's worth of work (see fs.h's
        // fs_write_range_step() contract), so this bounds each frame's
        // extra blocking time to a single block write, not the whole
        // file. On a terminal result the handle is already freed by
        // fs_write_range_step() itself -- clear the slot and hand the
        // outcome to whichever window started it, if it's still open
        // (see close_window()/bring_to_front() for why pending_write_win
        // is guaranteed to still be accurate here even if other windows
        // closed or reordered while this write was in flight).
        wmwd_phase("fs_steps");
        if (pending_write) {
            enum fs_step_result r = fs_write_range_step(pending_write);
            if (r != FS_STEP_PENDING) {
                void *finished_win = (pending_write_win >= 0 && pending_write_win < window_count)
                                          ? &windows[pending_write_win] : 0;
                const struct gui_app *app = finished_win ? windows[pending_write_win].app : 0;
                pending_write = 0;
                pending_write_win = -1;
                if (finished_win && app && app->on_write_complete) {
                    app->on_write_complete((struct window *)finished_win, r == FS_STEP_DONE);
                }
                redraw_pending = 1;
            }
        }

        // Same per-frame polling for a pending read (Milestone 1 phase
        // 4, docs/roadmap.md) -- mirrors the pending_write block just
        // above exactly, including the pending_read_win accuracy
        // guarantee from bring_to_front()/close_window(). The extra
        // `total` out-param (fs_read_range_step()'s only difference
        // from fs_write_range_step()) is handed to on_read_complete
        // alongside success/failure so the app knows how many bytes it
        // actually got.
        if (pending_read) {
            uint32_t total = 0;
            enum fs_step_result r = fs_read_range_step(pending_read, &total);
            if (r != FS_STEP_PENDING) {
                void *finished_win = (pending_read_win >= 0 && pending_read_win < window_count)
                                          ? &windows[pending_read_win] : 0;
                const struct gui_app *app = finished_win ? windows[pending_read_win].app : 0;
                pending_read = 0;
                pending_read_win = -1;
                if (finished_win && app && app->on_read_complete) {
                    app->on_read_complete((struct window *)finished_win, r == FS_STEP_DONE, total);
                }
                redraw_pending = 1;
            }
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

        if (pending_proc) {
            redraw_pending = 1;
            int exit_code = -1;
            enum sched_poll_result r = scheduler_poll(pending_proc, &exit_code);
            if (r != SCHED_POLL_RUNNING) {
                void *finished_win = (pending_proc_win >= 0 && pending_proc_win < window_count)
                                          ? &windows[pending_proc_win] : 0;
                const struct gui_app *app = finished_win ? windows[pending_proc_win].app : 0;
                pending_proc = 0;
                pending_proc_win = -1;
                if (finished_win && app && app->on_process_exit) {
                    app->on_process_exit((struct window *)finished_win, exit_code);
                }
            }
        }

        // Mouse movement alone normally takes wm_render_cursor_move()'s
        // cheap path below (cursor sprite only, not a full scene
        // repaint) -- fine everywhere except an open Start menu, whose
        // hover highlight is derived fresh from (mx, my) every full
        // repaint (see start_menu.c's start_menu_draw()). Force the
        // full path while the menu's open so hovering a different row
        // actually updates the highlight instead of only refreshing on
        // the next unrelated redraw.
        if (mouse_moved && start_menu_open) redraw_pending = 1;

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
        confirm_dialog_update_press(mx, my, buttons);
        file_picker_update_press(mx, my, buttons);
        if (!(buttons & 0x1)) {
            if (confirm_dialog_update_hover(mx, my)) redraw_pending = 1;
            if (file_picker_update_hover(mx, my)) redraw_pending = 1;
        }

        // Content hover, on the same only-when-the-mouse-moved cheap
        // path as the title-bar hover above. It also has to run once
        // after a button is RELEASED (the suppression inside it lifts
        // then, and the control under the cursor should light up again
        // without needing a further wiggle) -- hence the `|| !buttons`.
        if (mouse_moved || !(buttons & 0x1)) wm_update_content_hover(mx, my, buttons);

        wm_update_drag_resize(mx, my, buttons);
        desktop_update_drag(mx, my, buttons); // desktop icon drag, if one's in progress -- see desktop.h

        static uint64_t last_second = (uint64_t)-1;
        uint64_t ticks = pit_ticks();
        uint64_t this_second = ticks / 100;
        if (this_second != last_second) {
            last_second = this_second;
            tray_update_clock(); // also sets redraw_pending + damages the taskbar strip
        }

        // Esc used to always exit the window manager here -- replaced by
        // the Start menu's "Exit to shell" (see wm_system_actions above),
        // which is discoverable instead of a hidden key. Esc itself is
        // deliberately unclaimed at the WM level now, free for a future
        // per-window or modal use (e.g. canceling a confirm dialog)
        // instead of double-booking it as "exit everything".
        uint8_t key_mods = 0;
        int key = keyboard_try_getchar_mods(&key_mods);
        // A `gui key` from the debug console, if the real keyboard had
        // nothing -- deliberately second, so a human at the keyboard is
        // never pre-empted by a queued test keystroke.
        if (key == -1) {
            int injected = wm_debug_next_key_mods(&key_mods);
            if (injected) key = injected;
        }

        int wheel = mouse_get_wheel_delta();
        if (wheel == 0) wheel = wm_debug_next_wheel(); // `gui wheel`, same
                                                        // second-place rule as
                                                        // the injected key above

        // Both taps sit after their injected-input fallbacks above, so a
        // `gui key` / `gui wheel` reaches the compositor exactly as a
        // real one does.
        if (key != -1) compositor_raw(WIN_EV_RAW_KEY, key, 0, key_mods);
        if (wheel != 0) compositor_raw(WIN_EV_RAW_WHEEL, wheel, 0, 0);

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
                if (key == KEY_SUPER) {
                    if (!confirm_dialog_open && !file_picker_open) {
                        if (start_menu_open) start_menu_open = 0;
                        else start_menu_open_now();
                        context_menu_open = 0;
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
        if (redraw_pending) {
            redraw_pending = 0;
            wm_render_frame(mx, my);
        } else if (mouse_moved) {
            wm_render_cursor_move(mx, my);
        }

        wmwd_frame_end();
    }
}
