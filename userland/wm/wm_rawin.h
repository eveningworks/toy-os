#ifndef WM_RAWIN_H
#define WM_RAWIN_H

#include <stdint.h>

// Input for a ring-3 compositor: received as WIN_EV_RAW_* events and
// kept here, rather than polled off the drivers. See wm_rawin.c for the
// inversion this represents and what collapsed with it.
//
// The API is deliberately the SHAPE wm.c already used -- ask for a
// position, take a key, take a wheel delta -- so the frame loop did not
// have to be restructured around an event stream. What changed is where
// the answer comes from, not when the loop asks.

// Seeds the pointer at the screen's centre, as mouse_init() used to.
// Call once, after the screen geometry is known.
void wm_rawin_init(int screen_w, int screen_h);
void wm_rawin_clamp(int screen_w, int screen_h);

// Drains every queued input event. Call ONCE per frame, before asking
// anything below. Draining fully (rather than one event per frame) is
// load-bearing -- see wm_rawin.c.
void wm_rawin_pump(void);

// The pointer as of the last pump. Any output may be NULL.
void wm_rawin_mouse(int *out_x, int *out_y, uint8_t *out_buttons);

// The next key EVENT, or -1 if none, CONSUMING it. `out_down` is 1 for a
// press and 0 for a release; both may be NULL.
//
// **A QUEUE, NOT A SLOT, AND THAT CHANGED WITH RELEASES.** This held one
// key with a comment arguing that "two keys arriving inside one frame is
// not something the hardware can produce at a 100Hz tick" -- which was
// already optimistic under autorepeat and a slow frame, and is simply
// wrong now: a press and its release routinely land in the same pump,
// and so do a modifier and the key it modifies. The cost of losing one
// changed too. A dropped PRESS is a keystroke the user repeats; a
// dropped RELEASE is a key the client believes is held down forever,
// which in a game is a player who will not stop walking.
int wm_rawin_take_key(uint8_t *out_mods, int *out_down);

// The modifiers as of the last key. Replaces keyboard_mods_now() for
// the one caller that wants modifiers without consuming a key
// (desktop.c, deciding whether a click extends a selection).
uint8_t wm_rawin_mods_now(void);

// Accumulated wheel notches since the last call, CONSUMING them.
int wm_rawin_take_wheel(void);

#endif
