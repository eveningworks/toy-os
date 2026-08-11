#ifndef THEME_H
#define THEME_H
#include "gfx.h"

// Named colors for the handful of RGB triples that keep getting
// re-typed identically across the GUI apps (wm_render.c, calculator.c,
// notepad.c, about.c) -- e.g. the same light-gray button background
// shows up in both Calculator and Notepad, the same near-black text
// color shows up in three different files. These are function-like
// macros, not precomputed constants: gfx_rgb() converts to the active
// framebuffer's native pixel format, so each use still calls it exactly
// as before -- this is a naming layer, not a caching one, and swapping
// a call site over to one of these changes nothing about what gets
// drawn.
//
// Deliberately not exhaustive: only values that were already identical
// in two or more places got a name here. A color used once in one file
// (say, the close button's red, or the title bar's focused blue) stays
// as a plain gfx_rgb() call at its call site -- giving it a THEME_*
// name here wouldn't remove any duplication, just add a layer of
// indirection for no reason. Add the next name only when a value
// actually repeats.
//
// This is also apps-internal, like wm/wm.h and widgets.h -- not part of
// kapi.h. Include it directly wherever it's needed.

#define THEME_WHITE      gfx_rgb(255, 255, 255) // pure white: display/text backgrounds, light icon strokes
#define THEME_TEXT       gfx_rgb(20, 20, 20)    // near-black body text, used against light backgrounds
#define THEME_BORDER     gfx_rgb(60, 60, 60)    // window/menu border lines
#define THEME_BUTTON_BG  gfx_rgb(225, 225, 230) // light gray button background (Calculator's number keys, Notepad's toolbar)
#define THEME_WINDOW_BG  gfx_rgb(235, 235, 235) // default window content background
#define THEME_PANEL_BG   gfx_rgb(245, 245, 245) // slightly lighter panel background (Calculator's window, the Start menu)
#define THEME_SELECTION_BG gfx_rgb(51, 144, 255) // text-selection highlight (ui_scrollback.c's click-drag/shift-arrow selection)

#endif
