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

// Registers an ICON item -- the panel's own, not an app's (wm.h offers
// text items only). `icon` is an icon NAME kept by the caller, so it
// must be a string literal or other static storage. Returns a handle,
// or -1 when the tray is full.
int tray_register_icon(const char *icon);

// Should the item named by `desktop.tray_<key>` be shown, given whether
// its hardware is there? The item owner still calls tray_set_hidden();
// this only resolves the policy, so both tray items answer it the same
// way and a third needs no new code.
int tray_want_shown(const char *key, int hardware_present);

// Takes an item out of the strip WITHOUT freeing its slot, so its
// neighbours keep their order when it comes back (see the struct's
// note). Cheap enough to call every frame -- a no-op when unchanged.
void tray_set_hidden(int tray_id, int hidden);
int  tray_is_hidden(int tray_id);

// Swaps an icon item's picture -- the volume item's speaker changing
// with the level. A no-op when the name is unchanged, so this may be
// called every frame without repainting the taskbar.
void tray_set_icon(int tray_id, const char *icon);

// Any item's box in the taskbar strip, from the SAME right-to-left walk
// that draws it -- so a click cannot be told a different position from
// the one the item was drawn at. 0 when there is no such item.
int tray_item_rect(int tray_id, int *out_x, int *out_y, int *out_w, int *out_h);

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
