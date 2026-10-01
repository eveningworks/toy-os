#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

#include "wm.h"
#include "gui_apps.h"
#include "win_proto.h" // WIN_EV_*/struct win_event -- client windows, see wm_client.c
#include "ui/ugfx.h"   // the screen surface every drawing call targets
#include <stdint.h>

// --- the screen ------------------------------------------------------
//
// The ring-0 WM drew through `gfx_*`, which had ONE implicit target: the
// framebuffer, reached through a file-global in gfx.c. A ring-3
// compositor owns its back buffer explicitly (M41's R1), so every
// drawing call names the surface it draws into -- which is also what
// lets the same `uui_*` widget serve a window client and the desktop.
//
// One screen per process, initialised once by wm_render_init(). It is a
// global rather than threaded through every call because it genuinely is
// process-wide state, and threading a surface pointer through 169 call
// sites and every helper between them would be noise that says nothing.
extern struct ugfx_screen g_wm_screen;
// WIN_EV_SCREEN: re-map the grant and re-lay out for the new size (wm.c).
void wm_screen_changed(void);
void wm_layout_changed(void); // the usable area moved: relay windows and the grid

// The drawable. Written as an accessor rather than a bare
// `&g_wm_screen.back` at each site so the indirection stays greppable
// and a future second surface (an off-screen compose target, say) is one
// function to change rather than 169.
// The scene's back buffer -- or, while a ghost is being snapshotted
// (wm_anim.c), the ghost's own buffer, so the chrome code draws there
// without knowing.
extern struct ugfx_surface *g_wm_surface_override;
static inline struct ugfx_surface *wm_surface(void) {
    return g_wm_surface_override ? g_wm_surface_override : &g_wm_screen.back;
}
// Draws `ghost`'s chrome, content and grip into `dst` at the ghost's
// own x/y (0, 0 for a snapshot). wm_render.c.
void wm_render_window_into(struct ugfx_surface *dst, struct window *ghost, int focused);

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
// (gfx_char_w/h) rather than fixed pixel constants; see the git history
// entries on why fixed sizes went stale once the font became
// runtime-selectable. Used by both wm_input.c (hit-testing these exact
// regions) and wm_render.c (drawing them), which is why they live here
// instead of being static in just one of those files.
#define START_LABEL "Start"
// The Start button's mark, when `desktop.start_button` asks for one --
// a NAME under /usr/share/icons like any other icon, not a path.
#define START_ICON  "start"
#define WIN_LABEL_MAX_CHARS 14 // how many chars of a window's title a labelled taskbar button budgets for

int start_btn_w(void);
int win_btn_w(void);

// A new window's place in OPEN order, stamped into `open_seq` where it
// is created. windows[] is z-order and changes on every raise; the
// taskbar lists by this instead, so a click cannot move its button.
uint32_t wm_next_open_seq(void);
// The window stamped `seq`, as an index into windows[] NOW, or -1. The
// way to hold a window across a frame: an index is reused by the next
// close_window(), which compacts the array.
int wm_window_by_seq(uint32_t seq);

// The window menu (Minimize/Restore, Maximize, Fullscreen, Close) for
// windows[idx] at (mx, my) -- the title bar's right-click, and the
// taskbar button's.
void wm_open_window_menu(int idx, int mx, int my);
int btn_size(void);

// THE RESIZE BORDER IS MOSTLY OUTSIDE THE WINDOW, which is how
// Windows and KWin make it big without spending window pixels on it
// (SM_CXPADDEDBORDER; KWin's resize-only border). The frame keeps 2
// rows; the other 8 hang over whatever is behind.
//
// The inside number is small ON PURPOSE. The top edge lies over the
// title bar, so every row it claims stops dragging the window -- at 8
// the corner reached 16 rows into a 23-row bar and the diagonal came up
// at the bar's own MIDPOINT. Outside costs nothing, so that is where
// the target lives.
//
// **THE OUTSIDE BAND STEALS FROM WHATEVER IS BEHIND IT**, including
// another window's content, and that is the bargain every desktop with
// an invisible border makes. It is bounded two ways: a window that
// cannot be resized has no band at all (wm_find_resize_zone()), and the
// band is only consulted where the topmost window at that point has
// one.
#define RESIZE_OUTSIDE 8
#define RESIZE_INSIDE  2

// Along the perpendicular axis, how far from a corner BOTH its edges
// are live -- measured inward from the frame, with the outside band on
// top of it, so a corner is about 16x16 of aimable target.
#define RESIZE_CORNER  8

// WHICH EDGES A RESIZE IS DRAGGING, as a mask. This is the shape
// Wayland's xdg_toplevel.resize_edge uses (and _NET_WM_MOVERESIZE
// before it): four edges, and a corner is two of them at once, so the
// code below never enumerates eight cases.
#define WM_EDGE_LEFT   0x1
#define WM_EDGE_RIGHT  0x2
#define WM_EDGE_TOP    0x4
#define WM_EDGE_BOTTOM 0x8
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

// --- the pointer, drawn somewhere that is not the back buffer ---------
//
// Both exist for WIN_REQ_SCREENSHOT and are two halves of one question:
// is the pointer in the frame that was captured? With a software cursor
// it is, and _erase() takes it back out; on the hardware cursor plane it
// is not, and _into() puts it in. See wm_screenshot.c.
// Leaves one client's windows out of the next rendered frame, so a
// screenshot tool can stay out of its own picture (WIN_SHOT_NO_SELF).
// Set it, render, capture, clear it, repaint -- wm_screenshot.c.
void wm_render_hide_pid(int pid);
int  wm_render_hidden_pid(void);

void wm_render_cursor_into(struct ugfx_surface *dst, int ox, int oy);
int  wm_render_cursor_erase(struct ugfx_surface *dst, int ox, int oy);

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

// The pending_write/pending_read/pending_proc slots are GONE (M41 stage
// 4c, R9). They existed only because a ring-0 wm_run() must never block
// on the filesystem, so a save became one block per frame through
// fs_write_range_step(); a ring-3 WM is a process and can just call
// read/write and be descheduled. The outcomes were delivered through
// gui_apps.h's on_write_complete/on_read_complete, which only a
// kernel-space app ever had -- and ring 0 has held no applications since
// stage 0, so the machinery was already dead when it was removed.

extern int resizing; // index into windows[], or -1 if not resizing
extern int resize_edges;   // WM_EDGE_* mask, the edges this drag moves
extern int resize_start_mx, resize_start_my;
extern int resize_start_w, resize_start_h;
// The LEFT and TOP edges move the window as well as size it, so the
// drag has to remember where it started from -- deriving it from the
// live x/y accumulates the rounding every clamp does.
extern int resize_start_x, resize_start_y;

// The interactive resize's ask (see wm.c). resize_ask_idx is -1 when
// there is none; the pid/id pair beside it is what makes a stale index
// detectable after a window closes.
extern int resize_ask_idx;
extern int resize_ask_pid;
extern unsigned resize_ask_win;
extern int resize_want_w, resize_want_h;
extern int resize_sent_w, resize_sent_h;
extern int resize_inflight;
extern uint64_t resize_sent_tick;
extern unsigned resize_asks;

// The outline a drag is showing, and which drag style is in force --
// see wm.c. wm_render.c reads the rect; wm_input.c owns the rest.
extern int drag_outline_win;
extern int drag_outline_x, drag_outline_y, drag_outline_w, drag_outline_h;
extern int move_outline_mode;
extern int resize_outline_mode;
extern int resize_auto;
extern unsigned resize_lag_ms;

// The client has PRESENTED a frame at the size it was asked for: send
// the next proposal if the drag has moved on. Called from wm_client.c's
// present handler -- see wm_resize_shown() on why not from the ack.
void wm_resize_shown(int idx, int size_changed);

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

// Which BUTTON bit armed content_pressed: 0x1 for the left button, 0x2
// for the right one. The press is released when THAT button goes up,
// not when the left one does -- without this a right-click forwarded to
// a client (see wm_handle_right_click()) would arm a press that nothing
// could ever release, and the client would sit holding a MOUSE_DOWN
// that never got its MOUSE_UP. Meaningless while content_pressed is -1.
extern int content_pressed_btn;

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
// Window damage is SHRUNK by a margin for the next `n` rendered frames
// (wm_render.c) -- the damage sweep's positive control, reached as
// `gui damage shrink <n>`.
void wm_damage_shrink(int n);
int  wm_damage_shrink_px(void);   // the inset to apply now, 0 when the lever is off

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
// bring_to_front() plus this window's dialogs, promoted above it. See
// wm.c for why the two are separate.
void raise_with_dialogs(int idx);
// The topmost TOPLEVEL (never a popup), or -1; and the window a key to
// that toplevel actually reaches -- its topmost popup, if it has one.
int wm_focus_index(void);
int wm_key_target(int focus);
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
// 100Hz the default is 3 seconds -- long enough that an app doing a
// slow disk read is not slandered, short enough that a user who has
// just pressed Alt+F4 is not left wondering. Windows uses 5s before
// ghosting a window; KDE's is comparable.
//
// A VARIABLE, not a constant, so `gui pingtimeout <ticks>` can move it
// -- the same shape as `gui watchdog <ms>`, and for a related reason.
// It is a TEST lever rather than a user setting: tools/forcequit_test.py
// waits out this timeout about ten times, which made it the slowest tool
// in the suite and therefore the suite's whole wall-clock floor. The
// timeout's VALUE is not what that tool is testing (its own docstring
// already treats raising it as a positive control), so nothing is lost
// by shortening it there and ~30 seconds is gained.
//
// Deliberately not a registered setting: it has no user-facing meaning
// worth a Control Panel row, and a persisted value would silently
// change how the desktop treats a slow app on every later boot.
#define WM_PING_TIMEOUT_DEFAULT 300
extern int wm_ping_timeout_ticks;

// HOW OFTEN EVERY CLIENT IS ASKED, not just one being closed. Without a
// cadence `not_responding` only ever appeared during a close attempt,
// which is the one moment a hang is least surprising -- and left the
// busy cursor with nothing to fire on.
//
// The cost is one wakeup per client per interval: a ping is an event,
// and an event wakes a client blocked in sys_wait_event(). 2s against
// the 30ms a Terminal already ticks at. Worst-case detection is
// interval + timeout, since a window is only asked once the previous
// answer has landed.
#define WM_PING_INTERVAL_DEFAULT 200

// How long the frame loop is willing to sleep with nothing else due, in
// milliseconds. This is the cadence of everything the compositor owes
// that NOBODY SENDS IT AN EVENT FOR: the tray clock, the client pings,
// the /etc generation polls, the Start menu's click flash, reaping a
// launched process.
//
// 100 ms rather than a frame time because none of that is animation --
// the clock renders whole seconds, the pings are on a 2 s cadence, and a
// generation poll is one integer compare. Anything that IS animation
// arrives as an event (input) or as a client timer, and the wait is
// clamped to the nearer of those.
#define WM_IDLE_WAIT_MS 100

extern int wm_ping_interval_ticks;

void wm_client_ping(struct window *win);

// TWP over a channel (lib/uwmchan.h): open it at startup, pump it AFTER
// the event queue every frame, and wait through it so a client's message
// and a kernel event both defeat the same park.
void wm_client_chan_open(void);
int  wm_client_chan_ready(void);
void wm_client_chan_pump(void);
void wm_client_chan_wait(int timeout_ms);

// Remember a pid this desktop launched, so wm_run() reaps its slot when
// it exits. See wm.c -- without this a Start-menu launch leaked a
// scheduler slot per open/close and the desktop stopped launching
// anything after four.
void wm_track_launched(int pid);

// Read back the launch table: slot i's pid (0 = free), and how many
// slots there are. Only wm_debug.c's `gui state` uses these.
int wm_launched_pid(int slot);
int wm_launched_max(void);

// Once per frame. Delivers WIN_EV_TIMER to each client whose repeating
// timer (TWP's WIN_REQ_TIMER) has come due, which is what lets a client
// animate or refresh on a schedule while BLOCKING in between instead of
// polling.
// The earliest armed client timer, in sys_monotonic_ns(), or 0 for none -- the frame
// loop's wait is clamped to it. See wm_client_next_timer_due().
uint64_t wm_client_next_timer_due(void);

void wm_client_check_timers(void);
// Re-sends the state events a client's inbox had no room for last time.
// Once per frame, after the timers.
void wm_client_flush_pending(void);
// The ping round trip, microseconds: the last, the worst, and the mean
// over `n` answers since the desktop started. What tools/ping_rtt.py
// reads through `gui compositor --json`.
void wm_client_ping_stats(unsigned long long *last_us, unsigned long long *max_us,
                          unsigned long long *avg_us, unsigned *n);
// The same round trip in TSC cycles: comparable across two builds on
// one host, and finer than the clocksource under an emulator, where
// the ns figure is quantised to a 10 ms tick.
void wm_client_ping_cycles(unsigned long long *last, unsigned long long *max,
                           unsigned long long *avg);
// What a FRAME cost, microseconds: the last, the worst and the mean over
// `n`, plus the mean in TSC cycles. `full` picks the bucket -- a
// full-screen repaint or a damage-limited one, which differ by more than
// an order of magnitude, so there is no combined figure on purpose. See
// wm_render.c for what is deliberately not counted.
void wm_frame_stats(int full, unsigned long long *last_us, unsigned long long *max_us,
                    unsigned long long *avg_us, unsigned long long *avg_cyc, unsigned *n);
void wm_frame_stats_reset(void);

const struct wmwd_dist *wm_client_ping_dist(void);
void wm_client_ping_reset(void);

// Once per frame. Returns the index of a window that has just gone
// unresponsive while being asked to close (the only case worth a
// dialog), or -1.
int wm_client_check_liveness(void);

// Last frame's damage rect, for wm_debug.c's `gui state`. w/h <= 0
// means "no damage reported -- full-screen repaint".
void wm_debug_damage(int *out_x, int *out_y, int *out_w, int *out_h);

// How many SCENE repaints have happened -- wm_render_frame() calls, not
// the cheap cursor-only path. `gui state` reports it so a test can tell
// "this input repainted something" from "the pointer sprite moved".
uint32_t wm_scene_frames(void);

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

// The app icon at the FAR LEFT of a title bar -- Windows' system-menu
// icon, KWin/Breeze's and XFWM's window-menu button. Returns the
// decoded picture (borrowed from icon_cache.h -- never freed, never
// held across an icon_cache_invalidate()) and fills the square it
// occupies, or NULL when this window shows none.
//
// ONE FUNCTION FOR BOTH BECAUSE DRAWN AND CLICKABLE MUST BE THE SAME
// RECT. wm_render.c blits what this returns and wm_input.c hit-tests
// what it filled in, so an icon that fails to decode (Crash Test ships
// with no file on purpose) yields no rect either -- rather than a
// clickable square with nothing in it, which is what a separate
// "where would it go" helper would have produced.
struct uimg;
const struct uimg *title_icon(int idx, int *out_x, int *out_y, int *out_size);

// Which .desktop icon name a window's app_id resolves to, or NULL --
// see wm.c. The taskbar and the title bar both ask.
const char *wm_window_icon_name(int idx);
const char *wm_window_icon_name_of(const struct window *w); // by app_id, no index

// The Start button's mark and its rect, or NULL when the button shows
// the word instead (`text` mode, or artwork missing from the disk).
// Same drawn-and-reported-are-one-answer rule as title_icon() above.
const struct uimg *start_icon(int *out_x, int *out_y, int *out_size);

// Which resize cursor (if any) to show -- WM_CURSOR_H/V are the
// straight-edge cases (one edge alone), and the two DIAG kinds are the
// corners, one per diagonal axis. wm_render.c draws the
// matching hand-drawn icon; wm.c's wm_run() loop doesn't care about
// this, it's purely a rendering decision made each frame from
// wm_find_resize_zone()/the active resizing state below.
// The first four are the compositor's own conclusion from geometry it
// owns; TEXT and WAIT are a CLIENT'S request (WIN_REQ_CURSOR), and WAIT
// is also raised by the WM itself for a window that stopped answering.
// Two lists on purpose -- a client may not name a resize shape.
// WM_CURSOR_DIAG is the \ diagonal (top-left/bottom-right corners);
// WM_CURSOR_DIAG2 is the / one. Appended rather than inserted -- these
// are mapped to theme-file indices by name in cursor_theme.c, and the
// two orders are deliberately not assumed to match.
enum wm_cursor_kind { WM_CURSOR_NORMAL, WM_CURSOR_H, WM_CURSOR_V, WM_CURSOR_DIAG,
                       WM_CURSOR_TEXT, WM_CURSOR_WAIT, WM_CURSOR_DIAG2 };

// Finds which window (if any) the point (mx, my) is over a resize edge
// of -- the same topmost-window-wins hit-testing wm_handle_left_click()
// uses to decide whether a click starts a resize, factored out so
// wm_render.c can ask the identical question each frame to pick a
// cursor (hovering, not clicking). Skips minimized/maximized windows
// and any app with resizable == 0 (see gui_apps.h), same as the click
// handler. Returns the window index, or -1 if the point isn't over a
// resize zone of any window; on a hit, *out_edges is a WM_EDGE_* mask,
// two bits set at a corner. Defined in wm_input.c.
int wm_find_resize_zone(int mx, int my, int *out_edges);

// The shape that would be DRAWN at (mx, my) right now -- frame, client
// and overlay rules all applied. For `gui state`, so a test can ask what
// a client's WIN_REQ_CURSOR actually resolved to instead of trying to
// recognise a 15x21 sprite in a screenshot.
enum wm_cursor_kind wm_cursor_kind_at(int mx, int my);

// wm_input.c's entry points, called from wm.c's wm_run() loop.
void wm_handle_left_click(int mx, int my);
void wm_update_drag_resize(int mx, int my, uint8_t buttons);

// Resize a client window with no pointer involved -- `gui resize`, and
// the only way a test can drive a resize. See wm_input.c.
void wm_resize_client(int idx, int w, int h);

// Pulls a window back somewhere its title bar can be grabbed, if it is
// no longer reachable at all -- off the side, or entirely behind the
// taskbar, both of which dragging deliberately allows. A window that is
// merely hanging off an edge is left alone. See the definition.
// Returns 1 if it actually moved the window (i.e. it was unreachable),
// 0 if it was already fine and nothing changed.
int wm_ensure_reachable(int idx);

// Right-click dispatch (see context_menu.h) -- closes whatever popup is
// already open, then opens whichever context menu (if any) fits
// (mx, my)'s target: a Start menu row, a taskbar app button, a window,
// or the desktop background. A right-click over the taskbar's Start
// button or empty taskbar space shows nothing (no real action to offer
// there yet). Defined in wm_input.c.
void wm_handle_right_click(int mx, int my);
// The thumb buttons (WIN_MOUSE_BTN_SIDE/EXTRA), delivered to the client
// under the pointer and to nothing else -- see the definition.
void wm_handle_thumb_button(int mx, int my, unsigned btn, int down);

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
int wm_point_on_desktop(int mx, int my);  // nothing on top of the desktop there

// Recomputes title_hover_win/kind from the live mouse position -- a
// no-op while a button is armed (title_btn_armed_win >= 0), since the
// press visual takes priority then. Sets redraw_pending when the hot
// button actually changes, same contract as uui_button_group_press().
void wm_update_title_hover(int mx, int my);

// wm_render.c's entry points, called from wm.c's wm_run() loop: a full
// scene repaint, and a cheap cursor-only-moved repaint (see
// wm_render_cursor_move()'s own comment in wm_render.c for why that split
// exists).
void wm_render_frame(int mx, int my);
void wm_render_cursor_move(int mx, int my);

// Has the shape changed since it was last drawn? A client answers a
// motion event frames later, so the shape moves while the mouse is
// still -- and the cheap path above only runs on a move. A COMPARISON,
// not a dirty flag somebody has to remember to set.
int wm_cursor_shape_changed(int mx, int my);

// wm_client.c -- the WM acting as the window server for ring-3 clients.
// Registers/unregisters itself with kernel/proc/win_server.c around
// wm_run(), so a client request outside GUI mode is refused rather than
// dispatched into a desktop that isn't running.
// Claims the compositor role, which gates the framebuffer grant and raw
// input delivery. Returns 1 on success, 0 if refused -- another process
// may already hold it. See wm_client.c.
int wm_claim_compositor(void);

// One client-request event, turned back into the callback the ring-0
// kernel used to make. Returns 1 if it was a client request, 0 if the
// event was something else (raw input, which wm_rawin.c owns).
int wm_client_handle_event(const struct win_event *ev);

// Runs one pending `gui` command, if any. Called once per frame --
// POLLED rather than event-driven, so a busy client cannot flood the
// diagnostic channel out of a lossy queue. See wm_client.c.
void wm_client_poll_debug(void);

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
void wm_client_send_key_up(struct window *win, int key, unsigned mods);
void wm_client_route_phys_key(int keycode, int down, unsigned mods);
void wm_client_send_mouse(struct window *win, int type, int x, int y, unsigned buttons);
void wm_client_send_close(struct window *win);

// Tell a client its window gained (1) or lost (0) keyboard focus.
void wm_client_send_focus(struct window *win, int focused);

// THE POPUP GRAB (abi/win_proto.h's WIN_REQ_POPUP). `owner` is the pid
// whose popup is topmost, or 0 when none is up. `route` says where a
// press at (mx, my) lands for that owner: 1 = in one of its popups,
// 2 = in the content of another of its windows, 0 = anywhere else --
// and 0 is what `dismiss` is for, which closes every popup of the pid
// and tells it with WIN_EV_POPUP_DONE.
// --- dialog windows (abi/win_proto.h's WIN_REQ_DIALOG, wm.h) ---------
//
// `wm_dialog_blocker` is the index of the MODAL dialog that owns
// windows[idx]'s input, or -1 -- what a press on a blocked window is
// redirected to. `wm_dialog_of` walks an owner's dialogs, modal or not,
// from index `after` (pass -1 to start). `wm_dialogs_raise` promotes
// every dialog of one owner above it.
int wm_dialog_blocker(int idx);
int wm_dialog_of(int idx, int after);
void wm_dialogs_raise(int pid, uint32_t owner_win);

int wm_client_popup_owner(void);
int wm_client_popup_route(int owner, int mx, int my);
void wm_client_popups_dismiss(int owner);

// Deliver wheel notches to a client (WIN_EV_WHEEL).
void wm_client_send_wheel(struct window *win, int notches);

// Propose a content size to a client (WIN_EV_RESIZE). A proposal, not a
// command -- see wm_client.c and abi/win_proto.h.
void wm_client_send_resize(struct window *win, int w, int h);
// The lease, told to the client (WIN_EV_SCANOUT): on with the grant's
// pitch, scanout count and first back index, or off.
void wm_client_send_scanout(struct window *win, int on, uint32_t pitch, int count, int back);

// Fullscreen: enter or leave, for a resizable window (wm_input.c).
void wm_set_fullscreen(int i, int on);
int  window_has_chrome(const struct window *win);

// wm_scanout.c -- the lease policy. Called once per rendered frame;
// while it says the display is leased the compositor draws nothing.
void wm_scanout_update(void);
int  wm_scanout_active(void);
int  wm_scanout_lessee(void);   // the pid, or 0
void wm_scanout_client_presented(int pid);   // an ex-lessee's pages may come off
// The topmost toplevel is fullscreen and has adopted the screen's size:
// nothing under it -- wallpaper, taskbar, other windows -- is drawn or
// clickable (wm_render.c).
int  wm_top_covers_screen(void);

// wm_watchdog.c -- the slow-frame watchdog. Times one loop iteration by
// phase and logs anything over the threshold. Read that file's top
// comment before changing where the phases are marked: it measures only
// the work AFTER the frame's `hlt`, which is what makes "nothing logged
// during a visible freeze" a real answer (the stall is below us) rather
// than a missing measurement.
// wm_hwcursor.c -- the hardware cursor plane (WIN_REQ_FB_CURSOR).
// sync() per resolved shape: 1 = the plane shows the pointer and the
// software sprite must not draw; 0 = software's turn (no plane, a
// shape the plane cannot hold, or the request failed). invalidate()
// after a theme or size change; active() is what the last sync said.
int  wm_hwcursor_available(void);
int  wm_hwcursor_sync(enum wm_cursor_kind kind);
int  wm_hwcursor_active(void);
void wm_hwcursor_invalidate(void);
void wm_hwcursor_hide(void);

// wm_render.c: the built-in arrow's coverage masks, for the plane --
// the only built-in shape that exists as masks rather than draw calls.
void wm_builtin_arrow_masks(const unsigned char **outline,
                             const unsigned char **fill,
                             int *w, int *h, int *stride);

// A latency distribution, in microseconds. Bucket i counts values in
// [2^i, 2^(i+1)) us; the last one is everything above. A log2 histogram
// resolves a percentile only to a factor of two, which is the right
// trade here -- the question this answers is idle-vs-loaded, and those
// differ by orders of magnitude (Android's gfxinfo framestats buckets
// for the same reason). The reader interpolates inside a bucket.
#define WMWD_BUCKETS 22
struct wmwd_dist {
    uint32_t n;
    uint64_t sum_us;
    uint64_t max_us;
    uint32_t bucket[WMWD_BUCKETS];
};
void wmwd_dist_add(struct wmwd_dist *d, uint64_t us);

void wmwd_park_begin(uint32_t asked_ms); // called immediately before the wait
void wmwd_frame_begin(void);
void wmwd_phase(const char *name);
void wmwd_frame_end(void);
void wmwd_set_threshold_ms(uint32_t ms); // 0 disables the LOG, not the counters
uint32_t wmwd_threshold_ms(void);
uint32_t wmwd_slow_frames(void);
uint32_t wmwd_peak_ms(void);
void wmwd_reset(void);                   // counters only; the threshold stays
const struct wmwd_dist *wmwd_work(void);  // per-frame work
const struct wmwd_dist *wmwd_wake(void);  // how late the wait returned

#endif
