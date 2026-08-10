#ifndef WIDGETS_H
#define WIDGETS_H

#include <stdint.h>

// Tiny shared "clickable rectangle" primitives, pulled out after three
// independent reimplementations of the same idea turned up: wm.c's
// title-bar buttons, Calculator's button grid, and Notepad's toolbar.
// One shared implementation now backs all three -- see
// apps/wm/wm_render.c, apps/calculator.c, and apps/notepad.c.
//
// Deliberately minimal: a filled rect with an optional centered text
// label, plus a hit test. That's everything all three current callers
// actually needed; it is NOT a general widget-toolkit start (no focus
// management, no layout engine, no scrollbars/text fields yet) -- add
// the next primitive here only once a second real caller needs it, the
// same "don't build it until something needs it" approach the rest of
// this codebase uses.
//
// Icon-only buttons (wm.c's minimize/maximize/close, which draw
// hand-drawn vector icons, not text): pass label=NULL to widget_button()
// to get just the background fill, then draw the icon yourself with your
// own gfx_* calls afterward -- see wm_render.c's draw_window_chrome().

// 1 if (px, py) falls inside the x/y/w/h rect, 0 otherwise. Whatever
// coordinate space the caller's other numbers are already in (screen
// pixels for wm.c, window-content-relative for Calculator/Notepad) --
// this doesn't care, it's just a bounds check.
int widget_hit(int x, int y, int w, int h, int px, int py);

// Fills the rect with `bg`, then -- if `label` is non-NULL -- centers it
// in `fg` on `bg` using the current font (gfx_char_w()/gfx_char_h(), so
// it stays correct across gfx_set_font_size() calls same as everything
// else that draws text). Pass label=NULL for an icon-only button.
void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg);

#endif
