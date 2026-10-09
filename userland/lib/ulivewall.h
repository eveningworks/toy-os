#ifndef ULIB_ULIVEWALL_H
#define ULIB_ULIVEWALL_H

// LIVE WALLPAPERS: the effects, their options, and a wallpaper program's
// whole main(). Each program in LIVEWALL_DIR is one line calling
// ulivewall_main(); System Settings draws its gallery previews through
// ulivewall_render() from the same table, so a preview cannot drift
// from what the desktop shows.
//
// The compositor runs the chosen program as its BACKGROUND client
// (userland/wm/wm_background.h) -- Wayland's background layer, swaybg's
// job -- and decides when it may draw: no frames while a window fills
// the screen or a screensaver runs.
//
// **IT DRAWS A GRID AND INTERPOLATES.** An effect fills a colour per
// cell corner and one bilinear pass paints the screen from it, so the
// per-pixel cost is a few adds -- what lets a full-screen background
// move at all under emulation. Nothing here calls a sine per pixel.
//
// Options are the screensavers' arrangement over a third pair of paths:
//   LIVEWALL_DESC_DIR/<name>.wallpaper   what an effect lets you change
//   LIVEWALL_CONF_DIR/<name>.conf        what it is set to

#include "lib/usaver.h"
#include "ui/ugfx.h"
#include <stddef.h>
#include <stdint.h>

#define LIVEWALL_DIR       "/bin/wm/wallpapers"
#define LIVEWALL_DESC_DIR  "/usr/wm/wallpapers"
#define LIVEWALL_CONF_DIR  "/etc/wallpapers"
#define LIVEWALL_DEFAULT   "aurora"
// ANIMATED PICTURES are live wallpapers too: `desktop.wallpaper_live`
// lists this directory's GIFs beside the effects, and the player plays
// one (argv[1], its name). The player is in a SUBDIRECTORY so it is not
// a choice itself -- a ChoiceDir lists files only.
#define LIVEWALL_ANIMATED_DIR "/usr/share/wallpapers/animated"
#define LIVEWALL_GIF_PLAYER LIVEWALL_DIR "/players/gif"
// VIDEOS are live wallpapers the same way: a .mpg or .avi in that
// directory, played silent and looping by this player (lib/uvid.h).
#define LIVEWALL_VIDEO_PLAYER LIVEWALL_DIR "/players/video"
static const char *const LIVEWALL_VIDEO_EXT[] = { "mpg", "avi", 0 };

// The grid pitch on the desktop, in pixels. A preview passes a smaller
// one so a thumbnail is not a handful of blobs.
#define ULIVEWALL_CELL 8
#define ULIVEWALL_PARTS 48

// One running effect. A caller owns the storage (the Settings page keeps
// one per preview tile); everything allocated is freed by close.
struct ulivewall {
    int effect;            // index into the table, -1 for none
    int speed;             // option `speed`, 1..10; 4 is the authored pace
    int palette;           // option `colours`, an index into the palettes
    uint64_t t_ms;         // effect time, already scaled by speed
    uint32_t rng;
    uint32_t *grid;        // (gw + 1) x (gh + 1) corner colours
    int gw, gh;
    uint16_t *heat;        // Ember's fire, gw x (gh + 2)
    uint64_t sim_ms;       // Ember: effect time the fire has been stepped to
    int32_t part[ULIVEWALL_PARTS][4]; // Fireflies: x, y (16.16 of the screen), phase, rate
};

// Opens effect `name` with its saved options. 0 for a name the table
// does not know; `w` is still safe to step, render and close.
int ulivewall_open(struct ulivewall *w, const char *name);
void ulivewall_close(struct ulivewall *w);

// Re-reads the options without restarting the effect's clock.
void ulivewall_reload(struct ulivewall *w, const char *name);

// Advances by `dt_ms` of wall time, scaled by the speed option.
void ulivewall_step(struct ulivewall *w, uint32_t dt_ms);
// Paints one frame filling `s`, on a grid of `cell` pixels.
void ulivewall_render(struct ulivewall *w, struct ugfx_surface *s, int cell);

// `name`'s declared options and where its values are written.
int ulivewall_options(const char *name, struct usaver *out);
void ulivewall_conf_path(const char *name, char *out, size_t cap);

// THE PLAIN COLOURS a background can be (`desktop.background_colour`):
// one table for the desktop that paints them and the Settings swatches.
// An unknown word is the first, the desktop's long-standing blue.
uint32_t ulivewall_colour(const char *word);

// A wallpaper program's main(): a resizable client that steps on a
// timer and draws. Returns uapp_run()'s status.
int ulivewall_main(const char *name);

#endif // ULIB_ULIVEWALL_H
