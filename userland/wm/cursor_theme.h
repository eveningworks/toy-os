#ifndef WM_CURSOR_THEME_H
#define WM_CURSOR_THEME_H

#include <stdint.h>
#include "wm_internal.h" // enum wm_cursor_kind

// Cursor THEMES: the pointer's shapes, as data files rather than code.
//
// A shape is two coverage masks -- an outline and a fill -- plus a
// hotspot. The COMPOSITOR supplies the colours at draw time, which is
// the whole reason the masks carry coverage and not colour: one shape
// set then serves a light theme and a dark one, and the two things
// people change most often (the shape and the colour) stay independent.
// X11's image+mask cursors have always split it this way, and the
// arrow already in this WM was built like that before themes existed.
//
// **Where the layers sit**, which is the part worth keeping straight
// once the WM moves to ring 3 (Milestone 41): an APP names a shape, the
// COMPOSITOR owns the theme and turns a name into pixels, and the
// display layer owns any hardware plane. That is what Windows'
// SetCursor(HCURSOR) and Wayland's cursor-shape-v1 both settled on
// after the alternative -- every client loading the theme itself -- was
// tried and abandoned. Nothing here talks to the kernel: the pointer is
// composited into the framebuffer the WM already draws into, so this
// whole file moves to ring 3 unchanged.
//
// **The hardware cursor is deliberately not part of this.** It is
// switched off on the only driver that has one (see docs/decisions.md),
// so the drawn sprite is the only pointer any reachable configuration
// has. When virtio-gpu makes a plane real (M27a), it consumes the same
// masks -- that is why the shape is a mask pair and not a draw call.

// Every shape a theme may provide. A theme need not be complete: a
// missing or unparseable shape falls back to the built-in one, so a
// half-written theme degrades to a working pointer rather than to none.
//
// The names are the STABLE part -- they are what an app will eventually
// ask for, and a rename breaks every theme on disk. Six because those
// are the ones with a caller: four the WM already resolves, plus `text`
// (the moment a pointer crosses an editable field) and `wait`. An
// unknown name in a theme directory is ignored, so adding a seventh
// later costs nothing.
#define CURSOR_SHAPE_COUNT 7
const char *cursor_shape_name(int index); // "arrow", "resize-h", ...

// The authored size cap. Generous for a pointer (the default theme's
// largest is 15x21) and the bound on the static storage below, which is
// CURSOR_SHAPE_COUNT of these.
#define CURSOR_SHAPE_MAX 32

struct cursor_shape {
    int loaded;              // 0 means "use the built-in fallback"
    int w, h;
    int hot_x, hot_y;        // the pixel that IS the pointer position
    unsigned char outline[CURSOR_SHAPE_MAX][CURSOR_SHAPE_MAX];
    unsigned char fill[CURSOR_SHAPE_MAX][CURSOR_SHAPE_MAX];
};

// Parses one shape file. Returns 1 on success, 0 on any malformation --
// and REJECTS rather than guessing, per this repo's parser convention: a
// half-parsed cursor is a shape that draws wrong forever, where a
// refusal falls back to the built-in and is visible in the log.
//
// Takes a BUFFER, not a path, so it is pure and testable with no
// filesystem -- and so the ring-3 version of this file needs no change
// beyond who does the reading.
int cursor_shape_parse(const char *text, uint32_t len, struct cursor_shape *out);

// Loads every shape of `theme` from /usr/share/cursors/<theme>/.
// Missing shapes are left unloaded rather than failing the whole theme.
// Returns how many loaded, so 0 means "nothing found" -- which is not an
// error either, it just means the built-ins are what you get.
int cursor_theme_load(const char *theme);

// The shape for `kind`, or NULL if it did not load and the caller should
// use its built-in. Never returns a partially-filled shape.
const struct cursor_shape *cursor_theme_shape(enum wm_cursor_kind kind);

// The integer scale the size setting asks for, at least 1. Nearest
// neighbour at whole multiples, because a pointer wants a hard edge and
// a scaled-up mask with soft edges reads as blurry rather than large.
int cursor_theme_scale(void);

// Reads the `cursor_theme` and `cursor_size` settings and reloads if
// either changed. Cheap enough for once a frame: it compares the
// settings generation counter and does no I/O unless it moved.
void cursor_theme_poll(void);

// Registers both settings and loads the configured theme. Called once
// from the WM's startup.
void cursor_theme_init(void);

#endif
