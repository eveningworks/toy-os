#ifndef FONT_ATLAS_H
#define FONT_ATLAS_H

#include <stdint.h>
#include "ttf.h"
#include "font_ttf.h"

// Turning a parsed TrueType face into the ATLAS everything on this
// machine draws from -- the one implementation, compiled twice.
//
// **WHY THIS IS SHARED SOURCE.** Ring 0 builds an atlas today
// (font_face.c) and a ring-3 font service will build the same one, and
// the two must agree BYTE FOR BYTE: the compositor, every client and the
// console all index the same blob, so a cell one pixel taller in one
// ring is a desktop whose text disagrees with itself. Two
// implementations kept in step by review is exactly the drift this repo
// keeps deleting, so there is one, on the same shared-source path as
// ttf.c and geom.c beside it.
//
// **IT ALLOCATES NOTHING.** The caller supplies the output blob and the
// ~69 KB `struct ttf_scratch`, which is what lets one implementation
// serve ring 0, a ring-3 service and a test with no allocator in
// common. Measure first (font_atlas_plan) to learn how big the blob has
// to be, then render into it.
//
// **THE THREE SECTIONS ARE LAID OUT BACK TO BACK AND THE ORDER IS ABI**
// -- glyphs, advances, kern. A client is told where the glyphs and the
// advances start and DERIVES the kern table from `advances + count`, so
// reordering the blob silently hands every client the wrong table
// rather than failing. See struct font_atlas in api/font_face.h and
// WIN_REQ_FONT in abi/win_proto.h.

// What measuring decided, and everything rendering needs. Held by the
// caller so neither step allocates.
struct font_atlas_plan {
    int px;          // em size asked for
    int weight;      // enum font_weight, as asked
    int synthetic;   // 1 when bold will be SMEARED rather than loaded
    int smear;       // emboldening strength, 0 when not synthesizing

    int cell_w;      // the fixed cell: the WIDEST advance in the set
    int cell_h;      // rows per glyph bitmap -- the INDEXING stride
    int line_h;      // rows between lines -- what LAYOUT uses
    int baseline;    // rows from the cell top to the baseline
    int count;       // == FONT_TTF_GLYPH_COUNT
    int monospace;   // 1 when every advance is equal

    uint64_t glyph_bytes; // count * cell_w * cell_h
    uint64_t total_bytes; // glyphs + advances + kern, what the blob needs

    // Resolved once here so rendering does not re-do the cmap lookups.
    int gids[FONT_TTF_GLYPH_COUNT];
    uint8_t advances[FONT_TTF_GLYPH_COUNT];
};

// The codepoint a slot holds. ASCII 32..126, then the baked extras --
// index into this order IS the glyph index everything uses.
uint32_t font_atlas_slot_codepoint(int slot);

// Measures `t` at `px` and fills `p`. `t` is the face whose outlines
// will actually be rendered: pass the BOLD file when there is one, and
// set `synthesizing` when there is not and the regular outlines are to
// be smeared instead.
//
// `tight` asks for the terminal cell (the baked tables' squeezed
// ascent and descent, clipping capital accents); 0 gives UI text the
// font's full ascent and descent.
//
// Returns 1, or 0 when the result would be unusable -- a line under two
// pixels, or a cell wider than the rasteriser's accumulator.
int font_atlas_plan(const struct ttf_font *t, int px, int weight,
                    int synthesizing, int tight, struct font_atlas_plan *p);

// Renders the plan into `out`, which must be at least `p->total_bytes`
// and ZEROED by the caller. `sc` is scratch the caller owns.
//
// Returns the number of glyphs that produced ink, which is diagnostic
// only -- a face with missing glyphs still yields a usable atlas.
int font_atlas_render(const struct ttf_font *t, const struct font_atlas_plan *p,
                      uint8_t *out, struct ttf_scratch *sc);

#endif
