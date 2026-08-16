#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

#include "wm.h"
#include "gui_apps.h"
#include "win_proto.h" // WIN_EV_*/struct win_event -- client windows, see wm_client.c
#include <stdint.h>

// Private to the window manager's own files (wm.c / wm_input.c /
// wm_render.c / start_menu.c) -- never included from outside apps/wm/,
// and NOT part of wm.h's public API. This is what makes the wm.c split
// honest about what it is: none of these are independent modules,
// they're the same tightly-coupled single-threaded event loop as
// before, just organized into separate files by concern (state +
// lifecycle + entry point / input handling / rendering / the Start
// menu popup) for readability. All of them still share the mutable
// state below directly.
//
// If you're adding a new file under apps/wm/, this is the header it
// needs; if you're adding a new *app* (a window's contents), you want
// wm.h instead -- see apps/README.md.

// How many window slots exist at startup. NOT a limit: the table grows
// on demand (wm_windows_reserve()), so the only ceiling on open windows
// is memory, the way it is on a real desktop -- Windows bounds windows
// by per-process USER handle quotas in the thousands, and X11 by a
// 29-bit resource id. This used to be a hard `#define MAX_WINDOWS 6`,
// and the seventh window was refused: politely for a ring-3 client
// (a create request that fails, which the protocol allows) and SILENTLY
// for anything opened from the Start menu, which simply did nothing.
//
// Sized so the common case never reallocates at all.
#define WM_WINDOWS_INITIAL 8

// Taskbar/title-bar button sizing -- derived from the current font
// (gfx_char_w/h) rather than fixed pixel constants; see the CHANGELOG
// entries on why fixed sizes went stale once the font became
// runtime-selectable. Used by both wm_input.c (hit-testing these exact
// regions) and wm_render.c (drawing them), which is why they live here
// instead of being static in just one of those files.
#define START_LABEL "Start"
#define WIN_LABEL_MAX_CHARS 7 // how many chars of a window's title the taskbar button shows

int start_btn_w(void);
int win_btn_w(void);
int btn_size(void);

#define RESIZE_MARGIN 6
#define MIN_CONTENT_W 120
#define MIN_CONTENT_H 80

// The shared state every file in apps/wm/ reads and mutates directly --
// defined (without `static`) in wm.c, declared `extern` here. See the
// comment above for why this is deliberately not hidden behind
// accessor functions: it's one event loop's state, not a boundary
// between independently-reasoned-about components.
// A grown-on-demand block, not a fixed array -- but still indexed as
// `windows[i]`, which is why every one of its ~230 use sites is
// unchanged. Two rules follow from it MOVING when it grows:
//
//   - **Never hold a `struct window *` across a call that can open a
//     window.** This was already the rule for a different reason (a
//     window's index changes when the z-order does -- see
//     docs/decisions.md's "struct window * isn't a stable per-window
//     identity"); growth makes the pointer itself stale rather than
//     merely pointing at the wrong window.
//   - Index, don't cache. `windows[i]` is always current.
extern struct window *windows;
extern int window_count;

// Makes room for at least `n` windows, growing the table if needed.
// Returns 0 only if the allocation failed, which is the ONLY reason a
// window can now be refused.
//
// Call it before appending; `windows` may be a different pointer
// afterwards.
int wm_windows_reserve(int n);

extern int screen_w, screen_h;
extern int taskbar_h;

// The Start menu popup itself (open/closed state, hover/flash, the app
// list + system actions) now lives in its own start_menu.h/.c -- see
// that header, which every apps/wm/ file that needs it includes
// directly, same as this one.

// Set by a system action (currently just "Exit to shell") to ask
// wm_run()'s main loop to return to the calling shell -- checked right
// after click handling, same place Esc used to be checked before it
// moved to a Start menu item.
extern int wm_exit_requested;

extern int dragging; // index into windows[], or -1 if not dragging
extern int drag_off_x, drag_off_y;

// Handle from fs.h's fs_write_range_begin() for a steppable write
// currently in flight, or NULL if none -- see wm.c's wm_run() loop,
// which polls it once per frame via fs_write_range_step() instead of
// blocking (Milestone 1 phase 3, docs/roadmap.md). Same "-1/NULL means
// none" idiom as dragging/resizing above: a WM-global single slot, not
// per-window, since only one steppable write is in flight at a time
// today (window_start_write() refuses a second one while one's already
// pending -- see wm.h). pending_write_win is the index into windows[]
// the write belongs to, used to look up which app's on_write_complete
// callback to invoke once polling reaches a terminal result; -1 when
// pending_write is NULL.
extern void *pending_write;
extern int pending_write_win;

// Handle from fs.h's fs_read_range_begin() for a steppable read
// currently in flight, or NULL if none -- mirrors pending_write/
// pending_write_win above exactly (Milestone 1 phase 4,
// docs/roadmap.md): a second WM-global single slot, polled once per
// frame via fs_read_range_step() in wm_run(), maintained across
// bring_to_front()/close_window() reordering the same way. A separate
// slot from pending_write, not a shared one, since a read and a write
// could in principle be in flight at the same time for two different
// windows -- nothing does that yet, but there's no reason to force
// "only one steppable I/O op at all" when the underlying fs.h API
// doesn't require it either. pending_read_win is the index into
// windows[] the read belongs to, used to look up which app's
// on_read_complete callback to invoke; -1 when pending_read is NULL.
extern void *pending_read;
extern int pending_read_win;

// pid from scheduler.h's scheduler_spawn() for a process currently in
// flight, or 0 if none -- Milestone 1 phase 4b (docs/roadmap.md), the
// Terminal async-spawn item. Same single-WM-global-slot shape as
// pending_write/pending_read above, but sentinel-typed differently:
// scheduler_spawn()/scheduler_poll() already use 0 as "no such
// process" (scheduler.h), so there's no separate NULL-vs-int
// distinction to make here the way pending_write's `void *` handle
// needed -- 0 IS the "none" value both this slot and the scheduler
// agree on. pending_proc_win is the index into windows[] the process
// belongs to, used to look up which app's on_process_exit callback to
// invoke; -1 when pending_proc is 0. Unlike pending_write/pending_read,
// there's no handle to free on a terminal result -- scheduler_poll()
// itself reaps the process's slot the moment it returns
// SCHED_POLL_EXITED, so there's nothing left to leak even if a caller
// never followed up.
extern int pending_proc;
extern int pending_proc_win;

extern int resizing; // index into windows[], or -1 if not resizing
extern int resize_right, resize_bottom;
extern int resize_start_mx, resize_start_my;
extern int resize_start_w, resize_start_h;

// The outline a client-window resize drag is proposing (see wm.c).
// -1/-1 when there is none.
extern int resize_prop_w, resize_prop_h;

// index into windows[], or -1 if no app is currently capturing a
// content-area drag via its on_drag_start callback (see gui_apps.h) --
// same shape as dragging/resizing above, just app-owned instead of
// WM-owned. First user: Terminal's scrollbar thumb (widgets.h's
// widget_scrollbar_*).
extern int content_dragging;

// index into windows[], or -1 -- same shape as content_dragging, but
// for gui_apps.h's on_press/on_release instead of on_drag_start/
// on_drag. The two are mutually exclusive per press (see on_click's own
// comment): a content-area button-down either starts a drag (claimed by
// on_drag_start) or a press sequence (on_press, if the app has one and
// nothing claimed the drag), never both. See wm_input.c's
// wm_handle_left_click() for where this gets set and
// wm_update_drag_resize() for where it's driven each tick.
extern int content_pressed;

// Title-bar minimize/maximize/close buttons: Windows/KDE-style delayed
// commit. Mouse-down on one of these ARMS it (title_btn_armed_win/kind)
// and shows a pressed visual -- the actual action (minimize/maximize/
// close) only fires on mouse-up while the cursor is STILL over that
// same button; dragging off before releasing cancels it with no effect,
// same as any real desktop's title-bar buttons. See wm_input.c's
// wm_update_title_btn_press() (drives all of this each tick, mirrors
// wm_update_drag_resize()'s content_pressed handling) and
// wm_render.c's draw_window_chrome().
//
// -1 for "none armed". title_btn_armed_kind: 0=minimize, 1=maximize,
// 2=close. title_btn_pressed_active is whether the cursor is currently
// over the armed button (drives the pressed-vs-unpressed visual while
// held, same "hot" concept as ui_button_group's `pressed` field).
extern int title_btn_armed_win;
extern int title_btn_armed_kind;
extern int title_btn_pressed_active;

// Hover highlight shown when nothing's armed -- which window/button (if
// any) is under the cursor right now, recomputed fresh every tick from
// the live mouse position (wm_update_title_hover()), the same "derive
// live, don't persist a stale answer" approach as the Start menu's own
// hover. -1/-1 when the cursor isn't over any title-bar button, or
// while a button is armed (the press visual takes over then -- see
// above).
// Index into windows[] of the window whose CONTENT area the cursor is
// currently over, or -1. Distinct from title_hover_win above (that one
// tracks the title BAR's own buttons, which the WM draws itself): this
// exists only so the WM knows which app to tell when the cursor leaves,
// since an app can't notice that on its own. See wm_update_content_hover().
extern int content_hover_win;

extern int title_hover_win;
extern int title_hover_kind;

extern int redraw_pending;

// Accumulates (x, y, w, h) into the pending scene-damage region
// wm_render_frame() will clip its next repaint to -- see wm_render.c's
// damage-region compositor (docs/decisions.md) and gfx_set_clip_rect()
// (gfx.h), which is what actually enforces it once wm_render_frame()
// applies the accumulated region as the active clip. Safe to call from
// anywhere in apps/wm/ any number of times before the next
// wm_render_frame() -- rects just accumulate into a single bounding
// box, the same "good enough, not a real rect list" tradeoff gfx.c's
// own dirty-pixel tracking already makes. Does NOT set redraw_pending
// itself -- callers still need to do that separately (or already do,
// for the same event); this only narrows WHERE the next repaint
// touches, not WHETHER one happens. Defined in wm_render.c.
void wm_damage_rect(int x, int y, int w, int h);

// Window lifecycle -- defined in wm.c, used by wm_input.c (opening from
// the Start menu, closing via the title-bar X).
void bring_to_front(int idx);
void open_app(const struct gui_app *app);
void close_window(int idx);

// ASK a window to close, the way a user asking it to would: a ring-3
// client gets WIN_EV_CLOSE and answers for itself, a kernel-space app is
// closed directly. **Every user-facing close goes through here** -- the
// title bar's X, the context menu's Close, and Alt+F4 -- so that a
// client's veto (uapp's on_close) holds whichever one the user reached
// for. It did not, before this existed: the context menu called
// close_window() straight, which tore a client's window down behind the
// process's back and skipped its handshake entirely.
//
// close_window() remains the unconditional teardown, for the WM's own
// use and for a client that has agreed (WIN_REQ_DESTROY).
void wm_request_close(int idx);

// --- client liveness (apps/wm/wm_client.c) ---------------------------
//
// How long an unanswered ping means "not responding". At the PIT's
// 100Hz this is 3 seconds -- long enough that an app doing a slow disk
// read is not slandered, short enough that a user who has just pressed
// Alt+F4 is not left wondering. Windows uses 5s before ghosting a
// window; KDE's is comparable.
#define WM_PING_TIMEOUT_TICKS 300

void wm_client_ping(struct window *win);

// Remember a pid this desktop launched, so wm_run() reaps its slot when
// it exits. See wm.c -- without this a Start-menu launch leaked a
// scheduler slot per open/close and the desktop stopped launching
// anything after four.
void wm_track_launched(int pid);

// Read back the launch table: slot i's pid (0 = free), and how many
// slots there are. Only wm_debug.c's `gui state` uses these.
int wm_launched_pid(int slot);
int wm_launched_max(void);

// Once per frame. Returns the index of a window that has just gone
// unresponsive while being asked to close (the only case worth a
// dialog), or -1.
int wm_client_check_liveness(void);

// Last frame's damage rect, for wm_debug.c's `gui state`. w/h <= 0
// means "no damage reported -- full-screen repaint".
void wm_debug_damage(int *out_x, int *out_y, int *out_w, int *out_h);

// Damage verification (debug): render every frame twice and report any
// pixel the damage-limited pass got wrong. See wm_render.c's own
// comment for the bug class it exists to catch. Off by default.
// Call when a GUI session starts: forces the next frame to be a full
// repaint rather than damage-limited. See wm_render.c.
void wm_render_reset(void);

void wm_damage_verify_set(int on);
int  wm_damage_verify_enabled(void);

// The title-bar button layout (minimize/maximize/close rects) -- needed
// by wm_input.c to hit-test clicks against and by wm_render.c to draw
// them, so it can't be static to either file. Defined in wm_render.c
// since it's fundamentally a layout/drawing computation.
struct btn_rects { int min_x, max_x, close_x, y, size; };
struct btn_rects title_buttons(const struct window *win);

// Which resize cursor (if any) to show -- WM_CURSOR_H/V are the
// straight-edge cases (dragging the right or bottom edge alone),
// WM_CURSOR_DIAG is the corner (both at once). wm_render.c draws the
// matching hand-drawn icon; wm.c's wm_run() loop doesn't care about
// this, it's purely a rendering decision made each frame from
// wm_find_resize_zone()/the active resizing state below.
enum wm_cursor_kind { WM_CURSOR_NORMAL, WM_CURSOR_H, WM_CURSOR_V, WM_CURSOR_DIAG };

// Finds which window (if any) the point (mx, my) is over a resize edge
// of -- the same topmost-window-wins hit-testing wm_handle_left_click()
// uses to decide whether a click starts a resize, factored out so
// wm_render.c can ask the identical question each frame to pick a
// cursor (hovering, not clicking). Skips minimized/maximized windows
// and any app with resizable == 0 (see gui_apps.h), same as the click
// handler. Returns the window index, or -1 if the point isn't over a
// resize zone of any window; on a hit, *out_right/*out_bottom say which
// edge(s) matched (both set means the corner). Defined in wm_input.c.
int wm_find_resize_zone(int mx, int my, int *out_right, int *out_bottom);

// wm_input.c's entry points, called from wm.c's wm_run() loop.
void wm_handle_left_click(int mx, int my);
void wm_update_drag_resize(int mx, int my, uint8_t buttons);

// Right-click dispatch (see context_menu.h) -- closes whatever popup is
// already open, then opens whichever context menu (if any) fits
// (mx, my)'s target: a Start menu row, a taskbar app button, a window,
// or the desktop background. A right-click over the taskbar's Start
// button or empty taskbar space shows nothing (no real action to offer
// there yet). Defined in wm_input.c.
void wm_handle_right_click(int mx, int my);

// Drives title_btn_pressed_active while a title-bar button is armed,
// and fires its action on release if the cursor's still over it -- see
// title_btn_armed_win's own comment above for the full contract. A
// no-op when nothing's armed (title_btn_armed_win == -1).
void wm_update_title_btn_press(int mx, int my, uint8_t buttons);

// Delivers gui_app's on_hover() to whichever window's content area the
// cursor is over, and (-1,-1) to the one it just left. No-op while any
// button is held or a press/drag is armed -- the press visual owns the
// drawing then, exactly as wm_update_title_hover() steps aside for
// title_btn_armed_win.
void wm_update_content_hover(int mx, int my, uint8_t buttons);

// Recomputes title_hover_win/kind from the live mouse position -- a
// no-op while a button is armed (title_btn_armed_win >= 0), since the
// press visual takes priority then. Sets redraw_pending when the hot
// button actually changes, same contract as ui_button_group_press().
void wm_update_title_hover(int mx, int my);

// wm_render.c's entry points, called from wm.c's wm_run() loop: a full
// scene repaint, and a cheap cursor-only-moved repaint (see
// wm_render_cursor_move()'s own comment in wm_render.c for why that split
// exists).
void wm_render_frame(int mx, int my);
void wm_render_cursor_move(int mx, int my);

// wm_client.c -- the WM acting as the window server for ring-3 clients.
// Registers/unregisters itself with kernel/proc/win_server.c around
// wm_run(), so a client request outside GUI mode is refused rather than
// dispatched into a desktop that isn't running.
void wm_client_init(void);
void wm_client_shutdown(void);

// 1 if this window's content is a ring-3 client's pixel buffer rather
// than a gui_app's on_draw(). The two are mutually exclusive (wm.h).
int wm_client_is_client_window(const struct window *win);

// Composites a client window's buffer into its content area. Called
// from wm_render_frame() in place of the app->on_draw() it doesn't
// have, and under the same content clip.
void wm_client_draw(const struct window *win);

// Deliver input to a client. No-ops for a non-client window, so call
// sites can stay branch-free where that reads better. `type` for the
// mouse one is a WIN_EV_MOUSE_* value; coordinates are screen
// coordinates and get converted to window-relative inside.
void wm_client_send_key(struct window *win, int key, unsigned mods);
void wm_client_send_mouse(struct window *win, int type, int x, int y, unsigned buttons);
void wm_client_send_close(struct window *win);

// Tell a client its window gained (1) or lost (0) keyboard focus.
void wm_client_send_focus(struct window *win, int focused);

// Deliver wheel notches to a client (WIN_EV_WHEEL).
void wm_client_send_wheel(struct window *win, int notches);

// Propose a content size to a client (WIN_EV_RESIZE). A proposal, not a
// command -- see wm_client.c and abi/win_proto.h.
void wm_client_send_resize(struct window *win, int w, int h);

#endif
