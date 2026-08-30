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
// The menu's own layout maths, exposed so nothing has to re-derive it.
// Rows run top to bottom: gui_app_registry_count app rows, then
// wm_system_action_count action rows. Row i spans
// [menu_y + i*item_h, menu_y + (i+1)*item_h).
//
// Public because apps/wm/wm_debug.c reports it over the debug console
// -- tools/gui_flow.py used to hardcode these numbers and they drifted.
// Valid whether or not the menu is currently open.
void start_menu_geometry(int *out_menu_x, int *out_menu_y, int *out_menu_w,
                          int *out_item_h, int *out_total_items);

void start_menu_open_now(void);

// Draws the popup at its fixed taskbar-anchored position, using the
// live mouse position for hover -- a no-op if start_menu_open is 0 (the
// caller in wm_render.c still checks that itself before calling, same
// as before this split, so this stays a pure "draw what's asked"
// function like every other wm_render.c drawing routine).
void start_menu_draw(int mx, int my);

// Tracks which row the cursor is over. Returns 1 when it CHANGED,
// having damaged the menu's own rect -- so a mouse move inside one row
// costs nothing and one across rows repaints the menu rather than the
// screen. Call once a frame; a no-op while the menu is closed.
// The overlay registry's hover op -- see wm_overlay.h. Returns the
// hovered row + 1 (0 for none) and adopts it; the core compares it and
// calls start_menu_damage().
int start_menu_hover_at(int mx, int my);

// Damages the rect the menu occupies, whether or not it is open --
// which is what makes "the rows it just vacated" declarable by the code
// that closes it.
void start_menu_damage(void);

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
