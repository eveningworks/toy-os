#ifndef START_MENU_H
#define START_MENU_H

#include <stdint.h>

// The Start menu popup: a CATEGORY SIDEBAR, the selected folder's apps
// beside it, and a search field across the bottom -- KDE's Kickoff and
// XFCE's Whisker in shape, with the search field at the foot as
// Windows 7 put it. It was one flat column of every app until the list
// outgrew it.
//
// The sidebar carries the folders and, below a divider, the system
// actions (Exit to shell, Restart, Shutdown), so a power action stays
// one click away instead of behind a folder -- Kickoff files those
// under a "Leave" tab, Windows 7 keeps them in the footer, and the
// footer is the one that does not make shutting down a two-step.
//
// TYPING GOES TO THE SEARCH FIELD WHENEVER THE MENU IS OPEN. There is
// no focus to move: the field is the only thing here that takes text,
// which is what Windows and KDE both do (start typing, results appear).
// The keys arrive through wm_overlay.h's `key` op -- an overlay owns
// the keyboard while it is up, as an xdg_popup's grab does.
//
// This is NOT an independent module with a clean boundary -- it still
// reaches into wm_internal.h for screen_h/taskbar_h/redraw_pending/
// gui_app_registry/open_app(), same as wm_input.c and wm_render.c
// already do. It's the same single-threaded window manager, split by
// concern one level further, same reasoning as wm.c's own top comment
// for why wm_input.c/wm_render.c share state through wm_internal.h
// instead of hiding it behind accessors.

struct start_action { const char *label; void (*on_select)(void); };
extern const struct start_action wm_system_actions[];
extern const int wm_system_action_count;

// Whether the popup is currently open -- read by wm_render.c (draw or
// not) and wm_input.c (route a click here first while open, and to
// pick the taskbar Start button's highlight color).
extern int start_menu_open;

// The menu's outer rect and its row pitch. Everything inside is derived
// from these two by the row walker below, so nothing outside this file
// re-derives where a row is -- tools/gui_flow.py used to hardcode those
// numbers and they drifted. Valid whether or not the menu is open.
void start_menu_geometry(int *out_x, int *out_y, int *out_w, int *out_h,
                         int *out_item_h);

// What a reported row IS. The sidebar's folders and actions are always
// present; app rows are the SELECTED folder's, or the search results
// while a query is typed; the search field reports itself as a row so a
// test can click it without knowing the layout.
enum start_row_kind {
    START_ROW_CATEGORY = 0,
    START_ROW_ACTION,
    START_ROW_APP,
    // The one-line description of whatever row the pointer or the
    // keyboard is on, from the entry's `Comment=`. A status line, not a
    // control: it is reported so a test can read it, and it is never
    // hit-tested.
    START_ROW_DESC,
    START_ROW_SEARCH,
};

// Walk every row the menu currently DRAWS, in one call per row: the
// categories, the system actions, the visible app rows, then the search
// field. Returns 0 past the end. Any out pointer may be NULL.
//
// `selected` is 1 for the current folder and for the keyboard-
// highlighted app row -- the two things a screenshot cannot tell from a
// hover.
int start_menu_row_count(void);
int start_menu_row_info(int n, const char **label, int *kind,
                        int *x, int *y, int *w, int *h, int *selected);

// Where an app row's icon is drawn, and how big -- 0 for any other
// row. For a test comparing those pixels against the source file; the
// offsets are this file's and nothing else should re-derive them.
int start_menu_row_icon(int n, int *x, int *y, int *sz);

// What the description strip currently says: the `Comment=` of the row
// under the pointer, or of the keyboard's selection, or "" when neither
// has one. Never NULL.
const char *start_menu_description(void);

// How the app column is scrolled: the first row SHOWN, and how many
// rows the open folder has in total. A folder taller than the pane
// scrolls (the wheel, the arrows, Page Up/Down), which is the one place
// a row index and a screen position stop being the same number.
void start_menu_scroll_state(int *out_first, int *out_total);

// The overlay registry's wheel op -- notches over the app column scroll
// it. Returns 1 when it consumed them.
int start_menu_wheel(int mx, int my, int notches);

// The typed query ("" when none) and the selected folder's label --
// state a test would otherwise have to read off pixels.
const char *start_menu_query(void);
const char *start_menu_category(void);

// The app under (mx, my), or NULL -- for the right-click menu, which
// offers "Open"/"Add to desktop" on an app row and dismisses the menu
// anywhere else.
struct gui_app *start_menu_app_at(int mx, int my);

// Opens it, closing every other dismissable overlay (wm_overlay.h).
// Named with the `_now` suffix so it doesn't collide with the
// `start_menu_open` state variable above. Opening RESETS the folder to
// the first one and clears the query: a menu that reopens where you
// left it makes the same click do different things on different days.
void start_menu_open_now(void);
// Closes it with no action -- the overlay table's close op. Safe to
// call when it is already closed.
void start_menu_close(void);

// Draws the popup at its fixed taskbar-anchored position, using the
// live mouse position for hover -- a no-op if start_menu_open is 0.
void start_menu_draw(int mx, int my);

// The overlay registry's hover op (wm_overlay.h): which control the
// pointer is over, as an opaque token, adopted as it is reported. The
// core compares it against the last one and calls start_menu_damage().
int start_menu_hover_at(int mx, int my);

// Damages the rect the menu occupies, whether or not it is open --
// which is what makes "the rows it just vacated" declarable by the code
// that closes it.
void start_menu_damage(void);
// Where it is, for wm_overlay.h's automatic damage. 0 when it has
// no rect to report.
int start_menu_rect(int *x, int *y, int *w, int *h);

// Handles a left-click at (mx, my) while the popup is open -- selects a
// folder, launches an app, runs a system action, or closes the popup if
// the click landed outside it. Returns 1 if the popup was open (whether
// or not the click hit a row) so wm_input.c knows to stop there instead
// of falling through to its own ordinary click handling; 0 if the popup
// wasn't open at all.
int start_menu_handle_click(int mx, int my);

// The overlay registry's key op. Printable characters go to the search
// field, arrows move the folder (left/right) and the highlighted result
// (up/down), Enter launches it, Esc clears the query and then closes.
// Returns 1 when the key was consumed. A key held with Ctrl/Alt/Super
// is NEVER consumed -- a global shortcut must not be shadowed by a menu
// that happens to be open.
int start_menu_key(int key, uint8_t mods);

// Advances the post-click flash and closes the popup once it's shown
// long enough -- call unconditionally every wm_run() tick. A no-op
// whenever nothing's flashing.
void start_menu_update(void);

#endif
