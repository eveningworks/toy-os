#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>

// The desktop background + icon grid -- what's actually behind every
// window, previously just wm_render_frame()'s bare gfx_clear() fill
// with nothing on it. Same peer-file pattern as start_menu.c/
// context_menu.c: shares the window manager's state through
// wm_internal.h rather than being an independently-reasoned-about
// module (see wm.c's top comment). Preliminary support, by explicit
// request -- see docs/roadmap.md for what's deliberately NOT here yet:
// real wallpaper images (blocked on the not-yet-built image decoder --
// this fills a plain color instead) and per-icon context menus (right-
// clicking the desktop shows a quick-launch menu regardless of whether
// the click landed on an icon -- see context_menu.h).
//
// One icon per apps/gui_apps.h `gui_app_registry[]` entry, in registry
// order, laid out on a real grid (apps/ui/ui_icon_grid.h's
// icon_grid_cell_rect(), wrapping at however many columns fit
// screen_w) -- each icon's {col, row} is per-icon state now (icon_col/
// icon_row below), defaulting to the old single left-edge column
// layout (col 0, row = registry index) the first time it's ever
// needed, and draggable to any other cell from there (desktop_handle_
// click()/desktop_update_drag(), via ui_icon_grid.h's icon_drag
// session). Positions persist across reboot in /etc/desktop.conf,
// keyed by app name so they survive gui_app_registry being reordered
// -- see desktop.c's desktop_load_positions()/desktop_save_position().
// A hand-drawn filled square with the app name's first letter stands
// in for a real icon glyph (no image decoder yet -- see above), with
// the label below it. Two icons dragged onto the same cell simply
// overlap -- no swap/displace logic, a future refinement if it ever
// matters in practice.

// Fills the area below the taskbar with the desktop background and
// draws the icon grid + hover/select highlight -- called from
// wm_render_frame() in place of its old bare gfx_clear() fill.
void desktop_draw(void);

// Per-tick update for an icon drag in progress -- called from wm.c's
// wm_run() loop every tick, same shape as wm_input.c's
// wm_update_drag_resize(): while the left button stays held, the
// dragged icon's live pixel position follows the cursor (drawn by
// desktop_draw() at that position instead of its cell); on release,
// the icon snaps to and commits whichever cell is nearest the drop
// point and its new position is persisted. A no-op when no drag is in
// progress (armed by desktop_handle_click() below).
void desktop_update_drag(int mx, int my, uint8_t buttons);

// Handles a left-click at (mx, my) that nothing else (a window, the
// taskbar, an open Start/context menu) claimed -- i.e. wm_input.c's
// old "clicked empty desktop, nothing to do" fallback. A single click
// on an icon selects it (highlight only); a second click on the SAME
// icon within DESKTOP_DOUBLE_CLICK_TICKS launches it, same
// double-click-to-open convention as a real desktop. A click that
// doesn't land on any icon clears the current selection. Always
// returns after acting -- there's nothing below this in the input
// priority chain to fall through to.
void desktop_handle_click(int mx, int my);

// Opens a quick-launch context menu (one row per gui_app_registry
// entry) anchored at (mx, my) -- called from wm_input.c's right-click
// dispatch when nothing else claimed the right-click either. Doesn't
// care whether (mx, my) landed on a specific icon; that's a future
// per-icon-menu refinement, not this round's scope (see this file's
// top comment).
void desktop_handle_right_click(int mx, int my);

#endif
