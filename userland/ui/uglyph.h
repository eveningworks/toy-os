#ifndef UUI_UGLYPH_H
#define UUI_UGLYPH_H

// ANY character of a font file, drawn at any size -- what the session
// font cannot do: it carries ASCII and Latin-1 only (font_ttf.h's 191
// slots), and a character map needs the arrow, the Greek and the box
// drawing a face actually has. Rasterized here, in ring 3, by
// lib/ttf.h from the file itself, and kept in a small cache, so a grid
// of glyphs costs a rasterization once and a tinted blit after.
//
// A face is a whole .ttf read into memory (a few hundred KB) and parsed
// once; ttf.h bounds-checks every read of it, as the font is a file
// anyone can drop into /usr/share/fonts.
#include <stddef.h>
#include <stdint.h>
#include "ui/ugfx.h"
#include "ttf.h"

struct uglyph_face {
    struct ttf_font f;
    uint8_t *data;
    size_t len;
    int ok;
};

// 1 on success. The caller owns `face` and closes it.
int  uglyph_open(struct uglyph_face *face, const char *path);
void uglyph_close(struct uglyph_face *face);

// Does the face have a glyph for `cp` (not the .notdef box)?
int  uglyph_has(const struct uglyph_face *face, uint32_t cp);

// Draws `cp` at an em of `px` in `color`: its ink centred ACROSS the rect,
// and down it on the face's own baseline -- so a row of g, x and A lines
// up as text does -- moved only as far as keeps the ink inside the rect.
// 1 when drawn; UGLYPH_NO_INK for a glyph with nothing to draw (a
// space), so a caller can show it some other way; 0 when the face has
// no glyph for it.
#define UGLYPH_NO_INK 2
int  uglyph_draw(struct ugfx_surface *s, struct uglyph_face *face, uint32_t cp, int px,
                 int x, int y, int w, int h, uint32_t color);

// A run of UTF-8 text in `face` at `px`, its baseline at `baseline`,
// starting at `x`, kerned as the face kerns it; returns the advance.
// Characters the face lacks are skipped by their advance of nothing.
int  uglyph_text(struct ugfx_surface *s, struct uglyph_face *face, const char *utf8, int px,
                 int x, int baseline, uint32_t color);
// The same run's width, drawing nothing.
int  uglyph_text_width(struct uglyph_face *face, const char *utf8, int px);
// The face's ascent at `px`: how far above the baseline its tallest
// letters reach, for placing a run in a box.
int  uglyph_ascent(const struct uglyph_face *face, int px);

// The FAMILY name the file gives itself ("DejaVu Sans Mono"), from its
// `name` table, into `out`; 0 when it has none readable.
int  uglyph_family(const struct uglyph_face *face, char *out, int cap);

// Forgets every cached glyph of `face` -- after closing it, so a later
// face at the same address cannot be served the old one's.
void uglyph_forget(const struct uglyph_face *face);

#endif
