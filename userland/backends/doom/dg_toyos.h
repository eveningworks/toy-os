#ifndef DG_TOYOS_H
#define DG_TOYOS_H

// The seam between toy-os's Doom app and doomgeneric's platform layer.
//
// **WHY THIS HEADER EXISTS AT ALL: TWO `KEY_*` VOCABULARIES THAT CANNOT
// SHARE A TRANSLATION UNIT.** `kernel/include/api/keyboard.h` and
// doomgeneric's `doomkeys.h` both define macros called `KEY_F2`,
// `KEY_F3`, `KEY_F4` and `KEY_F10`, with different values, and neither
// is ours to rename. A file including both gets a redefinition warning
// and then silently uses whichever won.
//
// So the two live in separate files and meet here:
//
//     userland/gui/apps/doom.c   toy-os side: api/keyboard.h, uapp,
//                                 ugfx. NEVER includes doomkeys.h.
//     userland/backends/doom/dg_toyos.c   doom side: doomkeys.h, doomgeneric.h.
//                                 NEVER includes api/keyboard.h.
//
// This header includes NEITHER, so both can include it. Keys cross it
// as evdev KEYCODES (abi/input_keys.h, `INPUT_KEY_*`), a third
// vocabulary neither side defines, so nothing needs copying.

#include <stdint.h>

// --- the window ------------------------------------------------------
//
// Doom renders 320x200 and doomgeneric scales it into DG_ScreenBuffer at
// whatever DOOMGENERIC_RESX/RESY say, picking an integer factor itself
// (i_video.c). 2x is the largest that leaves the window comfortably
// inside a 1280x720 desktop with chrome, and an INTEGER factor is the
// point: a non-integer scale of 320x200 pixel art shimmers.
#define DOOM_RESX 640
#define DOOM_RESY 400

// --- what the app gives the backend ----------------------------------

// One key transition, as toy-os reported it. `down` is 1 for a press and
// 0 for a release -- which is the whole reason this port was possible,
// and why WIN_EV_KEY_UP had to exist first: DG_GetKey()'s signature
// takes a `pressed` flag, so a press-only OS could not answer it.
struct dg_key_event {
    int code;   // an evdev keycode (abi/input_keys.h) -- a key POSITION
    int down;
};

// Queue one, from the app's on_phys_key. Safe to call before
// doomgeneric has started. Drops the oldest if the queue is full, which
// cannot happen at human typing rates against a per-frame drain.
void dg_push_key(int keycode, int down);

// Queue a release for every key still down -- the window lost focus, and
// no releases will come for them (abi/win_proto.h's WIN_EV_KEY_PHYS).
void dg_release_all(void);

// --- what the backend gives the app ----------------------------------

// Called from DG_DrawFrame when doomgeneric has a finished frame in
// DG_ScreenBuffer. The app repaints from it; the backend never touches
// a surface, because only the app has one.
void dg_set_frame_ready(void (*cb)(void *ctx), void *ctx);

// DG_ScreenBuffer, or NULL before doomgeneric_Create() has allocated it.
// 0x00RRGGBB per pixel, DOOM_RESX wide -- which is exactly what
// ugfx_blit() takes, so the app's paint is one call and no conversion.
const uint32_t *dg_frame_pixels(void);

// The game changed the window title (Doom does it per level). Supplied
// by the app, because the title belongs to the window and the window
// belongs to the app.
void dg_title_changed(const char *title);

// Run doomgeneric: allocate, initialise, load the WAD, and return once
// the game is ready to be ticked. Returns 0 on success. Everything it
// can fail at is a missing or unreadable WAD, which it reports itself.
int dg_start(int argc, char **argv);

// One frame of game. Called from the app's on_tick.
void dg_tick(void);

// The app's own sheet is over the game: pause a level in progress the
// way the Pause key does -- the music stops too -- and on 0 unpause it
// only if this paused it. A menu, a demo or the title loop is left alone.
void dg_hold(int on);
int dg_paused(void);        // the game is paused (by anyone)
int dg_menu_active(void);   // a menu -- or DOOM's own help -- is up
int dg_playing(void);       // a level is being played: no menu, pause or demo

// Puts `text` on the game's message line, as F5's "High detail" does --
// shown with messages switched off too, and only while a level is up.
// Kept by pointer: `text` must outlive the message.
void dg_message(const char *text);

#endif
