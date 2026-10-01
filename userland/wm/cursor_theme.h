#ifndef WM_CURSOR_THEME_H
#define WM_CURSOR_THEME_H

#include <stdint.h>
#include "wm_internal.h" // enum wm_cursor_kind
#include "lib/ucursor.h"

// Cursor THEMES: the pointer's shapes, as data files rather than code.
//
// A shape is one of two kinds -- Xcursor's split between a mask cursor
// and an ARGB one:
//   - MASKS: an outline and a fill coverage grid, coloured by the
//     COMPOSITOR at draw time (a black rim, the theme's white fill), so
//     one shape set serves a light theme and a dark one. `default` and
//     `bold` are masks.
//   - an IMAGE: straight-alpha ARGB in a QOI file beside the descriptor,
//     whose colours are its OWN -- a coloured set is artwork the
//     compositor must not repaint. Up to CURSOR_SCALE_MAX pre-rendered
//     scales, so `large` is redrawn art rather than doubled pixels.
//
// **Where the layers sit**: an APP names a shape, the COMPOSITOR owns
// the theme and turns a name into pixels, and the display layer owns any
// hardware plane -- Windows' SetCursor(HCURSOR) and Wayland's
// cursor-shape-v1. The plane (wm_hwcursor.c) is fed from these same
// shapes through cursor_shape_argb().

// Every shape a theme may provide. A theme need not be complete: a
// missing or unparseable shape falls back to the built-in one, so a
// half-written theme degrades to a working pointer rather than to none.
//
// The names are the STABLE part -- they are what an app asks for, and a
// rename breaks every theme on disk. Each has a caller; `hand`, `move`
// and `not-allowed` are cursor-shape-v1's pointer, move and not-allowed.
// An unknown name in a theme directory is ignored.
// The theme a machine with no `cursor_theme` stored gets. MUST MATCH
// /etc/settings.d/system.cursor_theme's Default=: the WM reads the conf
// file directly and never sees the descriptor, so the two only agree by
// being written the same.
#define CURSOR_THEME_DEFAULT "graphite"

#define CURSOR_SHAPE_COUNT 10
const char *cursor_shape_name(int index); // "arrow", "resize-h", ...

// The shape format itself -- struct cursor_shape, its parser, decoding
// an image -- is lib/ucursor.h, shared with System Settings' gallery.

// Loads every shape of `theme` from /usr/share/cursors/<theme>/, an
// image shape at the rendering for the current size setting. Missing
// shapes are left unloaded rather than failing the whole theme. Returns
// how many loaded, so 0 means "nothing found" -- which is not an error
// either, it just means the built-ins are what you get.
int cursor_theme_load(const char *theme);

// The shape for `kind`, or NULL if it did not load and the caller should
// use its built-in. Never returns a partially-filled shape. `hand`,
// `move` and `not-allowed` fall back to the theme's ARROW when the theme
// lacks them: they have no built-in drawing, and an arrow is what the
// pointer showed in those places before they existed.
const struct cursor_shape *cursor_theme_shape(enum wm_cursor_kind kind);

// The integer scale the size setting asks for, at least 1. Whatever the
// kind, a shape's drawn extent is w*scale by h*scale.
int cursor_theme_scale(void);

// How many SCREEN pixels each stored pixel of `s` covers: the size
// setting over the scale the art was rendered at -- 1 for an image that
// came pre-rendered at this size, the whole scale for masks. Nearest
// neighbour, because a pointer wants a hard edge.
int cursor_shape_step(const struct cursor_shape *s);

// The pixel at drawn offset (dx, dy) from the shape's top-left, in
// SCREEN pixels: an image's own ARGB, or a mask pair folded into
// straight ARGB as `fill_rgb` over a black rim. 0 outside the shape.
// ONE answer for the software sprite and the hardware plane.
uint32_t cursor_shape_argb(const struct cursor_shape *s, int dx, int dy,
                            uint32_t fill_rgb);

// Reads the `cursor_theme` and `cursor_size` settings and reloads if
// either changed -- a size change too, since an image theme decodes the
// rendering FOR that size. Cheap enough for once a frame: it compares
// the settings generation counter and does no I/O unless it moved.
void cursor_theme_poll(void);

// Registers both settings and loads the configured theme. Called once
// from the WM's startup.
void cursor_theme_init(void);

#endif
