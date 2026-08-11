#ifndef START_MENU_H
#define START_MENU_H

// The Start menu popup: the app list + system actions below it, with
// hover/click-flash feedback -- factored out of wm.c/wm_input.c/
// wm_render.c into its own component now that it had grown real state
// and behavior of its own (open/closed, which row is hovered, which
// row is mid-flash). See docs/decisions.md for the "why now" writeup.
//
// This is NOT an independent module with a clean boundary -- it still
// reaches into wm_internal.h for screen_h/taskbar_h/redraw_pending/
// gui_app_registry/open_app(), same as wm_input.c and wm_render.c
// already do. It's the same single-threaded window manager, split by
// concern one level further, same reasoning as apps/wm/wm.c's own top
// comment for why wm_input.c/wm_render.c share state through
// wm_internal.h instead of hiding it behind accessors.

struct start_action { const char *label; void (*on_select)(void); };
extern const struct start_action wm_system_actions[];
extern const int wm_system_action_count;

// Whether the popup is currently open -- read by wm_render.c (draw or
// not) and wm_input.c (route a click here first while open, and to
// pick the taskbar Start button's highlight color).
extern int start_menu_open;

// Width of the popup in pixels -- wide enough for the longest label
// currently in it (scans both gui_app_registry and wm_system_actions).
int start_menu_w(void);

// Opens the popup -- called from wm_input.c when the taskbar Start
// button is clicked. Named with the `_now` suffix so it doesn't collide
// with the `start_menu_open` state variable above.
void start_menu_open_now(void);

// Draws the popup at its fixed taskbar-anchored position, using the
// live mouse position for hover -- a no-op if start_menu_open is 0 (the
// caller in wm_render.c still checks that itself before calling, same
// as before this split, so this stays a pure "draw what's asked"
// function like every other wm_render.c drawing routine).
void start_menu_draw(int mx, int my);

// Handles a left-click at (mx, my) while the popup is open -- routes it
// to the right app/system-action row (or closes the popup if the click
// landed outside it), and starts that row's post-click flash the same
// way as before this split. Returns 1 if the popup was open (whether or
// not the click actually hit a row) so wm_input.c knows to stop right
// there instead of falling through to its own ordinary click handling;
// returns 0 if the popup wasn't open at all.
int start_menu_handle_click(int mx, int my);

// Advances the post-click flash and closes the popup once it's shown
// long enough -- call unconditionally every wm_run() tick. A no-op
// whenever nothing's flashing.
void start_menu_update(void);

#endif
