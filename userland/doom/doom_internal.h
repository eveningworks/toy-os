#ifndef DOOM_INTERNAL_H
#define DOOM_INTERNAL_H

// DOOM's parts beside the game window (userland/gui/apps/doom.c): the
// game data it can run and where that comes from (doom_wad.c), and the
// front end -- the launcher page, the game-data card with its download,
// and the F1 key sheet over the game (doom_front.c). Split from doom.c
// by concern, the userland/wm/ way: doom.c is the game's window, these
// are everything that is not the game.
#include <stdint.h>
#include "ui/uapp.h"
#include "ui/ugfx.h"

#define DOOM_WAD_DIR "/usr/share/doom"
#define DOOM_CONF    "/etc/doom.conf"

// --- the game data (doom_wad.c) ---------------------------------------

// One IWAD this port knows by name. `url` NULL: never downloaded, only
// found -- a retail file the person brings themselves.
struct doom_iwad {
    const char *file;       // its name in DOOM_WAD_DIR
    const char *name;       // what it is called on screen
    const char *detail;     // one line about it
    const char *url;        // where a download fetches it from, or NULL
    const char *sha256;     // what the download must hash to
    unsigned long size;     // the download's size, for the card
    const char *zip_member; // when `url` is a .zip: the WAD inside it
    const char *source;     // the host named on the card
};
extern const struct doom_iwad DOOM_IWADS[];
extern const int DOOM_IWAD_COUNT;

int doom_iwad_path(const struct doom_iwad *w, char *out, int cap);
int doom_iwad_present(const struct doom_iwad *w);
const struct doom_iwad *doom_iwad_named(const char *file);
// The one to play: /etc/doom.conf's `iwad=` if it is here, else the
// first present in table order. NULL when there is none.
const struct doom_iwad *doom_iwad_chosen(void);
int doom_iwad_choose(const struct doom_iwad *w);
// The URL a download uses: `mirror=` in /etc/doom.conf replaces all but
// the file's name, so a LAN copy (or a test's server) stands in for the
// origin. 0 when there is none to download.
int doom_iwad_url(const struct doom_iwad *w, char *out, int cap);

// Does `path` start like an IWAD? A file someone points the card at.
int doom_wad_is_iwad(const char *path);

// TITLEPIC, 320x200 0x00RRGGBB, black where the picture has no post.
// 0, or -1 for a WAD without one (out is then black).
#define DOOM_TITLE_W 320
#define DOOM_TITLE_H 200
int doom_wad_titlepic(const char *path, uint32_t *out);

// --- the front end (doom_front.c) -------------------------------------

// Called from on_open: shows the launcher page or the game-data card,
// or returns 1 when the game should simply start (show_page=off and a
// WAD is here, or arguments asked for a particular start).
int doom_front_open(struct uapp *a, int has_args);
int doom_front_up(void);      // a front-end view, not the game, fills the window
int doom_help_up(void);       // the F1 sheet is over the game

void doom_front_draw(struct uapp *a, struct ugfx_surface *s);
void doom_help_draw(struct uapp *a, struct ugfx_surface *s);
void doom_help_open(struct uapp *a);
void doom_help_close(struct uapp *a);

// Input while the front end or the sheet has it. Each returns 1 when it
// took the event.
int doom_front_key(struct uapp *a, int key, unsigned mods);
void doom_front_press(struct uapp *a, int x, int y, unsigned mods);
void doom_front_motion(struct uapp *a, int x, int y, unsigned buttons);
void doom_front_release(struct uapp *a, int x, int y, unsigned buttons);
int doom_front_user(struct uapp *a, int a0, int a1);   // the download's worker
void doom_front_leave(void);  // the game takes the window

// --- doom.c, for the front end ----------------------------------------

// Starts the game on `w`, leaving the front end.
void doom_start_game(struct uapp *a, const struct doom_iwad *w);

#endif
