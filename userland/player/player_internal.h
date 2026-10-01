#ifndef PLAYER_INTERNAL_H
#define PLAYER_INTERNAL_H

// THE AUDIO PLAYER'S SHARED STATE, split by concern the way
// userland/settings/ is: the playlist -- the folder, its tags and the
// play order (pl_list.c) -- and the stage -- the cover, its ambient
// colours and the spectrum (pl_stage.c); the app itself, its widgets
// and commands, is userland/gui/apps/player.c. One process, so these
// are plain globals.
#include <stdint.h>
#include <stddef.h>
#include "rt/sys.h"
#include "lib/uimg.h"
#include "ui/ugfx.h"
#include "ui/uambient.h"

#define PL_PATH_MAX 256
#define PL_MAX      128

struct pl_track {
    char name[64];           // the file, in the folder
    char title[64];          // the tag's, or the name without its extension
    char artist[64];         // "" when untagged
    char album[64];
    uint32_t ms;             // TLEN, else the codec's header, else 0
    char format[16];         // "MP3", "MIDI", "WAV"
};

// pl_list.c ---------------------------------------------------------------
extern char g_dir[PL_PATH_MAX];
extern struct pl_track g_tracks[PL_MAX];
extern int g_track_count;
extern int g_shuffle, g_repeat;

// Lists the playable files of `dir` (by probe, never by extension),
// sorted by name, with their tags. Returns the count.
int  pl_scan(const char *dir);
int  pl_index_of(const char *name);
void pl_path(int i, char *out, size_t cap);
// The track after (dir > 0) or before `cur` in PLAY order -- shuffled
// or not -- or -1 past either end without repeat.
int  pl_step(int cur, int dir);
// A new shuffled order, `first` at its head (the track playing now).
void pl_reshuffle(int first);
// "1:18", or "" when the length is unknown.
void pl_fmt_ms(char *out, size_t cap, uint32_t ms);

// pl_stage.c --------------------------------------------------------------
extern struct uambient g_amb;
// The cover for track `i` (-1: none): its embedded picture, else a tile
// coloured from its title; the ambient colours follow it.
void stage_set_track(int i);
// The cover, scaled to `size` px square (cached per size). NULL when
// none could be made.
const struct uimg *stage_cover(int size);
// The spectrum: `bands` levels 0..255 of what is playing now, eased so
// a bar falls rather than blinks. Returns 1 when any level moved.
#define STAGE_BANDS 16
int  stage_spectrum_tick(int playing);
extern uint8_t g_levels[STAGE_BANDS];

#endif
