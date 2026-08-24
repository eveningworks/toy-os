#ifndef WM_TRAY_H
#define WM_TRAY_H

#include <stdint.h>

// The taskbar's notification area: a small right-to-left strip of
// text items, drawn just left of (and including) the clock. Same
// "factored out once it grew a concern of its own" reasoning as
// start_menu.c -- this used to just be draw_clock_area(), a single
// hardcoded function in wm_render.c. Now the clock is itself the
// first tray item (registered by tray_init() below, refreshed once a
// second by tray_update_clock()), and any other GUI app can register
// its own item the same way via wm.h's tray_register()/tray_set_text()/
// tray_unregister() -- see that header for the public, app-facing half
// of this API. This header is the WM-internal half: the setup/draw
// hooks wm.c and wm_render.c call directly, same "not a real module
// boundary, just organized by concern" spirit as start_menu.h.

// Registers the built-in clock as tray item 0 -- call once from
// wm_run()'s setup, before entering the main loop.
void tray_init(void);

// Refreshes the clock's tray text from the current local time -- call
// once per wm_run() tick, exactly where the old once-a-second
// last_second check used to call draw_clock_area()'s own rtc_read_local().
void tray_update_clock(void);

// Draws every active tray item right-to-left, starting at the
// taskbar's right edge -- called from draw_taskbar() in place of the
// old draw_clock_area(). `taskbar_y`/`bg`/`fg` match draw_taskbar()'s
// own locals exactly.
void draw_tray(int taskbar_y, uint32_t bg, uint32_t fg);

// The clock's own box in the taskbar strip -- 0 when no clock is
// registered (which cannot happen after tray_init(), but a caller
// should not have to know that). Comes from the SAME right-to-left walk
// draw_tray() uses, so a click cannot be told a different position from
// the one the clock was drawn at. Two callers: wm_input.c hit-tests a
// left click against it to open the calendar, calendar_popup.c anchors
// its panel to it.
int tray_clock_rect(int *out_x, int *out_y, int *out_w, int *out_h);

// The x the leftmost tray item starts at -- the right edge of the strip
// the window buttons get. See the definition for why it re-walks the
// items rather than caching a width.
int tray_left(void);

#endif
