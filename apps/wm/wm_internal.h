#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

#include "wm.h"
#include "gui_apps.h"
#include <stdint.h>

// Private to the window manager's own files (wm.c / wm_input.c /
// wm_render.c) -- never included from outside apps/wm/, and NOT part of
// wm.h's public API. This is what makes the wm.c split honest about
// what it is: wm_input.c and wm_render.c are not independent modules,
// they're the same tightly-coupled single-threaded event loop as
// before, just organized into separate files by concern (state +
// lifecycle + entry point / input handling / rendering) for
// readability. All three still share the mutable state below directly.
//
// If you're adding a new file under apps/wm/, this is the header it
// needs; if you're adding a new *app* (a window's contents), you want
// wm.h instead -- see apps/README.md.

#define MAX_WINDOWS 6

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
int start_menu_w(void);
int btn_size(void);

#define RESIZE_MARGIN 6
#define MIN_CONTENT_W 120
#define MIN_CONTENT_H 80

// The shared state every file in apps/wm/ reads and mutates directly --
// defined (without `static`) in wm.c, declared `extern` here. See the
// comment above for why this is deliberately not hidden behind
// accessor functions: it's one event loop's state, not a boundary
// between independently-reasoned-about components.
extern struct window windows[MAX_WINDOWS];
extern int window_count;

extern int screen_w, screen_h;
extern int taskbar_h;

extern int start_menu_open;

// Which Start menu row (if any) is showing its post-click flash, or -1
// -- see wm.c's own comment on start_menu_flash_index for the full
// story. Read by wm_render.c's draw_start_menu() (takes visual priority
// over hover), set by wm_input.c's wm_handle_left_click() on a row hit,
// cleared by wm_update_start_menu_flash() (below) once it's shown long
// enough.
extern int start_menu_flash_index;
extern uint64_t start_menu_flash_until; // pit_ticks() deadline

// Start menu items below the app list -- see wm.c's top comment on
// wm_system_actions for why these are separate from gui_app_registry.
struct start_action { const char *label; void (*on_select)(void); };
extern const struct start_action wm_system_actions[];
extern const int wm_system_action_count;

// Set by a system action (currently just "Exit to shell") to ask
// wm_run()'s main loop to return to the calling shell -- checked right
// after click handling, same place Esc used to be checked before it
// moved to a Start menu item.
extern int wm_exit_requested;

extern int dragging; // index into windows[], or -1 if not dragging
extern int drag_off_x, drag_off_y;

extern int resizing; // index into windows[], or -1 if not resizing
extern int resize_right, resize_bottom;
extern int resize_start_mx, resize_start_my;
extern int resize_start_w, resize_start_h;

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

extern int redraw_pending;

// Window lifecycle -- defined in wm.c, used by wm_input.c (opening from
// the Start menu, closing via the title-bar X).
void bring_to_front(int idx);
void open_app(const struct gui_app *app);
void close_window(int idx);

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
// Closes the Start menu once start_menu_flash_index's deadline passes --
// a no-op the rest of the time (start_menu_flash_index == -1). See
// wm.c's comment on start_menu_flash_index.
void wm_update_start_menu_flash(void);

// wm_render.c's entry points, called from wm.c's wm_run() loop: a full
// scene repaint, and a cheap cursor-only-moved repaint (see
// wm_render_cursor_move()'s own comment in wm_render.c for why that split
// exists).
void wm_render_frame(int mx, int my);
void wm_render_cursor_move(int mx, int my);

#endif
