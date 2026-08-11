#ifndef DESKTOP_H
#define DESKTOP_H

// The desktop background + icon grid -- what's actually behind every
// window, previously just wm_render_frame()'s bare gfx_clear() fill
// with nothing on it. Same peer-file pattern as start_menu.c/
// context_menu.c: shares the window manager's state through
// wm_internal.h rather than being an independently-reasoned-about
// module (see wm.c's top comment). Preliminary support, by explicit
// request -- see docs/roadmap.md for what's deliberately NOT here yet:
// real wallpaper images (blocked on the not-yet-built image decoder --
// this fills a plain color instead), per-icon context menus (right-
// clicking the desktop shows a quick-launch menu regardless of whether
// the click landed on an icon -- see context_menu.h), and rearranging/
// dragging icons.
//
// One icon per apps/gui_apps.h `gui_app_registry[]` entry, in
// registry order, in a single left-edge column -- there's no
// user-repositionable icon layout, same "derive from the existing
// registry, don't invent new per-icon state" reasoning start_menu.c's
// app list already uses. A hand-drawn filled square with the app
// name's first letter stands in for a real icon glyph (no image
// decoder yet -- see above), with the label below it.

// Fills the area below the taskbar with the desktop background and
// draws the icon grid + hover/select highlight -- called from
// wm_render_frame() in place of its old bare gfx_clear() fill.
void desktop_draw(void);

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
