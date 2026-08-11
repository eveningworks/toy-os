#ifndef UI_PRIMITIVES_H
#define UI_PRIMITIVES_H

#include <stdint.h>

// The two lowest-level "clickable rectangle" primitives everything else
// in apps/ui/ is built on -- pulled out after three independent
// reimplementations of the same idea turned up: wm.c's title-bar
// buttons, Calculator's button grid, and Notepad's toolbar. Originally
// lived in apps/widgets.c/.h (this codebase's very first shared GUI
// helper); moved here when every other widget in that file also moved
// into its own apps/ui/ file, so apps/widgets.c/.h no longer exist --
// see docs/decisions.md for why the whole directory looks the way it
// does now.
//
// Deliberately minimal: a filled rect with an optional centered text
// label, plus a hit test. `ui_button`/`ui_textbox` (this directory)
// both wrap widget_button()/widget_hit() to add owned geometry/state on
// top -- this file itself stays a bare stateless primitive, the same
// role it's always had.

// Shared text-cursor bar width -- a thin vertical bar, not a solid
// full-cell block, used by both ui_textbox's caret and ui_scrollback's
// cursor so the two read as the same "modern insert-point" shape
// everywhere text can be edited in the GUI.
#define CURSOR_BAR_W 2

// 1 if (px, py) falls inside the x/y/w/h rect, 0 otherwise. Whatever
// coordinate space the caller's other numbers are already in (screen
// pixels for wm.c, window-content-relative for Calculator/Notepad) --
// this doesn't care, it's just a bounds check.
int widget_hit(int x, int y, int w, int h, int px, int py);

// Fills the rect with `bg`, then -- if `label` is non-NULL -- centers it
// in `fg` on `bg` using the current font (gfx_char_w()/gfx_char_h(), so
// it stays correct across gfx_set_font_size() calls same as everything
// else that draws text). Pass label=NULL for an icon-only button.
//
// `pressed` draws a 2px inset border in a fixed dark color and nudges
// the label 1px down/right -- the classic "this button is currently
// held down" look. Deliberately doesn't darken `bg` itself: that would
// need unpacking an arbitrary already-packed pixel color back into
// r/g/b (gfx.c keeps that math private to its own blending code, see
// gfx_blend_pixel()), where an inset border needs no color math at all
// and reads just as clearly as "pressed." Callers that don't care pass
// 0 and get the exact same pixels either way.
void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg, int pressed);

#endif
