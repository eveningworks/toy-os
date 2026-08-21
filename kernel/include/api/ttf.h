#ifndef TTF_H
#define TTF_H

// A TrueType parser and scanline rasterizer, in fixed point.
//
// This is the runtime half of what tools/genttf.py does offline: given
// the bytes of a .ttf file it decodes glyph outlines (quadratic Bezier
// contours) and fills them into an 8-bit coverage map, which is the
// exact format font_ttf.h's baked tables already use -- so a glyph this
// produces is a drop-in for a glyph that was baked at build time (see
// kernel/drivers/font_face.c, which builds one atlas from either).
//
// THREE PROPERTIES ARE LOAD-BEARING, and each of them is why this file
// looks the way it does rather than like stb_truetype.
//
// **No floating point.** This kernel builds with -mno-sse and has no FP
// at all, so everything scaled is Q16.16 (fixed.h). Font units are
// 16-bit integers and unitsPerEm is typically 1000-2048, so a
// coordinate scaled to pixels has plenty of headroom in Q16.16 -- the
// one place that is not true is fx_mul of two large intermediates,
// which is why the edge walk keeps coordinates in pixel space rather
// than accumulating in font units.
//
// **No allocation.** Everything transient lives in a caller-supplied
// struct ttf_scratch (~70 KB, far too big for either ring's stack --
// the kernel budget is 16 KiB per stack and USERLAND_CFLAGS carries a
// -Wframe-larger-than too). That is what lets one implementation serve
// the kernel, a ring-3 program and a KTEST, the same rule geom.c and
// klineedit.c follow: this file may not reference anything kernel-only.
//
// **Every read is bounds-checked, because a font file is untrusted
// input.** Windows put exactly this code in ring 0 (win32k.sys/GDI) and
// spent a decade of CVEs on it before Win10 moved font parsing out to a
// sandboxed user-mode host; Linux never had it in the kernel at all,
// and under Wayland/X11 the client rasterizes. toy-os deliberately
// differs (the console needs glyphs before any process exists, and
// WIN_REQ_FONT shares one atlas so client text cannot drift from the
// desktop's) -- so the mitigation has to be in here instead: there is
// no raw pointer arithmetic into the file anywhere below, only rd_u8/
// rd_u16/rd_i16/rd_u32 with an explicit length check, and a malformed
// table makes a function return 0 rather than read past the buffer.
// docs/decisions/drivers.md has the full reasoning and the roadmap item
// for moving it to ring 3.
#include <stdint.h>
#include "fixed.h"

// Caps on one glyph's complexity. A Latin glyph in DejaVu Sans Mono
// peaks around 120 points and 40 contours' worth of nothing; these are
// generous by an order of magnitude and are REFUSALS, not truncations
// -- a glyph past any of them renders as blank rather than as a
// partial outline, because half an outline fills wrongly (the scanline
// fill needs closed contours) and a wrong glyph is worse than a
// missing one.
#define TTF_MAX_POINTS   1024
#define TTF_MAX_CONTOURS 64
#define TTF_MAX_EDGES    2048
#define TTF_MAX_CELL_W   256  // widest cell the coverage accumulator holds

// Vertical supersampling: each pixel row is sampled at this many
// sub-scanlines and the results averaged, while horizontal coverage is
// computed exactly from the span ends. 4 was chosen by looking at the
// result against the baked JetBrains Mono glyphs at 8px, where 1 is
// visibly aliased and 8 is indistinguishable from 4.
#define TTF_SUBSAMPLES   4

struct ttf_font {
    const uint8_t *data;
    uint32_t len;

    // Table offsets into `data`, 0 when the table is absent.
    uint32_t glyf, loca, cmap, hmtx, head, hhea, maxp;
    uint32_t glyf_len, loca_len;

    // The format-0 horizontal `kern` subtable's PAIR ARRAY, not the
    // table's own offset -- ttf_open() walks past the headers once so
    // that a lookup is a binary search and nothing else. 0 (with
    // kern_pairs 0) when the font has no kerning this understands,
    // which is the common case: a monospace face has none by
    // definition, and a modern proportional face may carry its kerning
    // only in GPOS (see ttf_kern_units()).
    uint32_t kern_pairs_off;
    int kern_pairs;

    int units_per_em;
    int loca_long;     // head.indexToLocFormat: 0 = u16 halves, 1 = u32
    int num_glyphs;
    int num_hmetrics;  // hhea.numberOfHMetrics
    int ascent;        // hhea, font units
    int descent;       // hhea, font units, POSITIVE (the file stores it negative)
    int line_gap;
};

struct ttf_edge { fx_t x0, y0, x1, y1; int dir; };

struct ttf_scratch {
    // One glyph's outline, already scaled into pixel space.
    fx_t    px[TTF_MAX_POINTS];
    fx_t    py[TTF_MAX_POINTS];
    uint8_t on[TTF_MAX_POINTS];      // 1 = on-curve point
    uint8_t flags[TTF_MAX_POINTS];   // the file's own point flags, repeat-expanded
    int     contour_end[TTF_MAX_CONTOURS];
    int     npoints, ncontours;

    struct ttf_edge edges[TTF_MAX_EDGES];
    int     nedges;

    fx_t    xs[TTF_MAX_EDGES];       // crossings of one sub-scanline
    int     xdir[TTF_MAX_EDGES];
    int32_t acc[TTF_MAX_CELL_W];     // coverage accumulator for one row
};

// Parses the table directory and the handful of headers everything else
// needs. Returns 1 on success, 0 for anything it does not understand or
// cannot bounds-check -- a TrueType collection (`ttcf`), a CFF/OpenType
// outline font (no `glyf`), a truncated file. `data` must stay alive and
// unmodified for as long as `f` is used; nothing is copied.
int ttf_open(struct ttf_font *f, const uint8_t *data, uint32_t len);

// Unicode codepoint -> glyph id, through `cmap`. Understands format 4
// (the BMP subtable every real font has) and format 12; returns 0 (the
// .notdef glyph) for an unmapped codepoint, which is also what a font
// says for one it has no glyph for -- the caller cannot distinguish
// them and does not need to.
int ttf_glyph_index(const struct ttf_font *f, uint32_t codepoint);

// Advance width of `gid` in font units (hmtx). Glyphs past
// numberOfHMetrics all share the last entry's advance, which is how a
// monospace font stores one advance for thousands of glyphs.
int ttf_advance_units(const struct ttf_font *f, int gid);

// The scale factor taking font units to pixels for a `px` em size.
static inline fx_t ttf_scale_for_px(const struct ttf_font *f, int px) {
    if (!f->units_per_em) return 0;
    return fx_div(fx_from_int(px), fx_from_int(f->units_per_em));
}

// Rounded pixel advance of `gid` at `px`. This is the number that makes
// proportional text possible at all -- see gfx_char_advance().
int ttf_advance_px(const struct ttf_font *f, int gid, int px);

// Kerning adjustment for the ordered pair (left, right) in font units,
// or 0 when the font does not kern that pair. Almost always negative:
// kerning tucks a pair closer than their advances alone would put them
// ("AV", "To"), which is why text that ignores it looks loose rather
// than broken.
//
// **Only the legacy format-0 horizontal `kern` subtable is read**, and
// that is a deliberate stopping point rather than an oversight. A
// modern OpenType face expresses kerning in GPOS, which is a shaping
// engine's job -- HarfBuzz is ~50k lines and exists because doing this
// properly means lookups, contextual rules and script logic. Reading
// `kern` is ~40 lines and covers what this OS actually draws: of the
// two faces shipped, liberation-sans carries a 908-pair format-0 table
// and dejavu-sans-mono has none at all (it is monospace, so kerning it
// would be wrong anyway). A face with GPOS-only kerning simply renders
// unkerned, exactly as it does today.
int ttf_kern_units(const struct ttf_font *f, int left_gid, int right_gid);

// The same adjustment rounded to pixels at `px`. Rounded per pair
// rather than accumulated in font units, because the caller adds it to
// an already-rounded advance -- carrying sub-pixel positions would mean
// every drawing path in both rings tracking a fractional pen, which is
// a much larger change than kerning is worth here.
int ttf_kern_px(const struct ttf_font *f, int left_gid, int right_gid, int px);

// SYNTHETIC BOLD: thickens an already-rasterized coverage cell in
// place, by OR-ing each row with itself shifted right by 1..`strength`
// pixels (taking the maximum coverage, so an antialiased edge stays
// antialiased). `strength` is clamped to 1..4.
//
// This is what GDI does when a family has no bold face, and what Cairo
// and DirectWrite fall back to for the same reason -- it is visibly
// worse than a designed bold, especially at small sizes where the
// smear closes a counter, and it is the only way a face somebody just
// dropped into /usr/share/fonts can have a bold at all. font_face.c
// prefers a real `<name>-bold.ttf` and reaches for this only when
// there is not one.
//
// The caller must leave room: emboldening widens a glyph by `strength`
// pixels, so a cell sized to the unemboldened advance clips. font_face.c
// adds it to the cell width and to every advance.
void ttf_embolden(uint8_t *cov, int w, int h, int strength);

// Rasterizes `gid` into `cov` (w*h bytes, row-major, 0 = background,
// 255 = fully ink), with the glyph origin at (x_origin, baseline) in
// that bitmap's own coordinates and y growing DOWNWARD (the font's y
// grows upward; the flip happens here).
//
// Returns 1 on success -- including for a glyph with no outline at all,
// which is what a space is, and which leaves `cov` untouched. Returns 0
// only for a malformed or too-complex glyph. Does NOT clear `cov`: the
// caller owns it and usually wants to clear a whole atlas once.
int ttf_render_glyph(const struct ttf_font *f, int gid, int px,
                     uint8_t *cov, int w, int h,
                     int x_origin, int baseline,
                     struct ttf_scratch *sc);

#endif
