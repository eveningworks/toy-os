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
// This header includes NEITHER, so both can include it. The `TOYKEY_*`
// values below are toy-os's codes written out as literals, which is a
// COPY and would normally be exactly the kind of thing this project
// refuses -- so doom.c static-asserts every one of them against the
// real `KEY_*` macro. That check compiles in the file that can see both
// this header and `api/keyboard.h`, which is the file that has no
// doomkeys.h to collide with. A drift in either direction is a build
// error rather than a key that quietly stops working.

#include <stdint.h>

// --- toy-os key codes, copied (and checked in doom.c) -----------------
//
// Only the ones this app translates. An ordinary printable character
// needs no entry: it arrives as itself and Doom's own key codes ARE
// ASCII for that range.
#define TOYKEY_ARROW_UP     0x91
#define TOYKEY_ARROW_DOWN   0x92
#define TOYKEY_PAGE_UP      0x93
#define TOYKEY_PAGE_DOWN    0x94
#define TOYKEY_ARROW_LEFT   0x95
#define TOYKEY_ARROW_RIGHT  0x96
#define TOYKEY_HOME         0x97
#define TOYKEY_END          0x98
#define TOYKEY_DELETE       0x99
#define TOYKEY_F2           0x9A
#define TOYKEY_F3           0x9B
#define TOYKEY_F10          0xA4
#define TOYKEY_F4           0xA5
#define TOYKEY_F1           0xAB
#define TOYKEY_F5           0xAC
#define TOYKEY_F6           0xAD
#define TOYKEY_F7           0xAE
#define TOYKEY_F8           0xAF
#define TOYKEY_F9           0xB0
#define TOYKEY_F11          0xB1
#define TOYKEY_F12          0xB2
#define TOYKEY_INSERT       0xB3
#define TOYKEY_PAUSE        0xB8
#define TOYKEY_SHIFT        0xA7
#define TOYKEY_CTRL         0xA8
#define TOYKEY_ALT          0xA9
#define TOYKEY_ALTGR        0xAA

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
    int code;   // a TOYKEY_* above, or a printable character
    int down;
};

// Queue one, from the app's on_key/on_key_up. Safe to call before
// doomgeneric has started. Drops the oldest if the queue is full, which
// cannot happen at human typing rates against a per-frame drain.
void dg_push_key(int code, int down);

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

#endif
