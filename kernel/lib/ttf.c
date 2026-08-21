// TrueType parsing and rasterization in fixed point. See api/ttf.h for
// what this is, the three properties that shape it (no floating point,
// no allocation, every read bounds-checked) and why a font parser in
// ring 0 needs that third one in particular.
//
// COMPILED TWICE, kernel and userland (Makefile's build/userland/shared/
// rule), the same way geom.c and klineedit.c are -- so it may not
// reference anything kernel-only. It includes fixed.h and <stdint.h> and
// nothing else, deliberately.
#include "ttf.h"
#include <stddef.h>

// --- bounds-checked reads --------------------------------------------
//
// THE ONLY WAY THIS FILE TOUCHES THE FILE BUFFER. Every one of these
// takes the absolute offset and answers 0 when the read would leave the
// buffer, so a truncated or hostile table produces zeros and a refusal
// upstream rather than a read of whatever follows the font in memory.
// There is deliberately no "and I know this is in range" fast path: the
// checks are two comparisons against a value already in a register, and
// the one time somebody skips them is the one time it matters.
static uint32_t rd_u8(const struct ttf_font *f, uint32_t off) {
    if (off >= f->len) return 0;
    return f->data[off];
}

static uint32_t rd_u16(const struct ttf_font *f, uint32_t off) {
    if (off + 2 > f->len) return 0;
    return ((uint32_t)f->data[off] << 8) | f->data[off + 1];
}

static int32_t rd_i16(const struct ttf_font *f, uint32_t off) {
    return (int32_t)(int16_t)(uint16_t)rd_u16(f, off);
}

static uint32_t rd_u32(const struct ttf_font *f, uint32_t off) {
    if (off + 4 > f->len) return 0;
    return ((uint32_t)f->data[off] << 24) | ((uint32_t)f->data[off + 1] << 16)
         | ((uint32_t)f->data[off + 2] << 8) | (uint32_t)f->data[off + 3];
}

static int in_range(const struct ttf_font *f, uint32_t off, uint32_t bytes) {
    return off <= f->len && bytes <= f->len - off;
}

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) \
                       | ((uint32_t)(c) << 8) | (uint32_t)(d))

int ttf_open(struct ttf_font *f, const uint8_t *data, uint32_t len) {
    if (!f || !data || len < 12) return 0;
    for (uint32_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;
    f->data = data;
    f->len = len;

    uint32_t ver = rd_u32(f, 0);
    // 0x00010000 is TrueType outlines; 'true' is the old Apple tag for
    // the same thing. 'OTTO' (CFF/Type2 outlines) and 'ttcf' (a
    // collection) are REFUSED rather than half-supported -- there is no
    // `glyf` in an OTTO font at all, so it would fail later and less
    // clearly. font_face.c reports the refusal by name.
    if (ver != 0x00010000u && ver != TAG('t', 'r', 'u', 'e')) return 0;

    uint32_t kern_off = 0, kern_len = 0;
    uint32_t num_tables = rd_u16(f, 4);
    if (num_tables == 0 || num_tables > 512) return 0;
    if (!in_range(f, 12, num_tables * 16)) return 0;

    for (uint32_t i = 0; i < num_tables; i++) {
        uint32_t rec = 12 + i * 16;
        uint32_t tag = rd_u32(f, rec);
        uint32_t off = rd_u32(f, rec + 8);
        uint32_t tlen = rd_u32(f, rec + 12);
        if (!in_range(f, off, tlen)) continue; // a table that does not fit is absent
        switch (tag) {
        case TAG('g', 'l', 'y', 'f'): f->glyf = off; f->glyf_len = tlen; break;
        case TAG('l', 'o', 'c', 'a'): f->loca = off; f->loca_len = tlen; break;
        case TAG('c', 'm', 'a', 'p'): f->cmap = off; break;
        case TAG('h', 'm', 't', 'x'): f->hmtx = off; break;
        case TAG('h', 'e', 'a', 'd'): f->head = off; break;
        case TAG('h', 'h', 'e', 'a'): f->hhea = off; break;
        case TAG('m', 'a', 'x', 'p'): f->maxp = off; break;
        case TAG('k', 'e', 'r', 'n'): kern_off = off; kern_len = tlen; break;
        default: break;
        }
    }

    if (!f->glyf || !f->loca || !f->head || !f->hhea || !f->maxp) return 0;

    f->units_per_em = (int)rd_u16(f, f->head + 18);
    f->loca_long = (int)rd_i16(f, f->head + 50);
    f->num_glyphs = (int)rd_u16(f, f->maxp + 4);
    f->ascent = (int)rd_i16(f, f->hhea + 4);
    f->descent = -(int)rd_i16(f, f->hhea + 6); // stored negative, kept positive here
    f->line_gap = (int)rd_i16(f, f->hhea + 8);
    f->num_hmetrics = (int)rd_u16(f, f->hhea + 34);

    // unitsPerEm outside this range is not a font this can scale
    // sensibly, and 0 would divide by zero in ttf_scale_for_px().
    if (f->units_per_em < 16 || f->units_per_em > 16384) return 0;
    if (f->num_glyphs <= 0) return 0;
    if (f->descent < 0) f->descent = 0;

    // `kern` is OPTIONAL, so nothing here may fail the open -- a font
    // whose kern table is malformed still draws, unkerned. That is the
    // opposite of the required tables above, and deliberate: refusing a
    // whole face over an advisory table would lose the user their font
    // for a defect in the one part of it that only affects spacing.
    //
    // Version 0 is the Microsoft/OpenType header (u16 version, u16
    // nTables). Apple's own `kern` starts with a u32 version 0x00010000
    // and a u32 nTables, and is NOT read -- the two are distinguishable
    // by that first u16 and only the Microsoft one appears in the fonts
    // this OS ships or is likely to be handed.
    if (kern_off && kern_len >= 4 && rd_u16(f, kern_off) == 0) {
        uint32_t ntab = rd_u16(f, kern_off + 2);
        uint32_t p = kern_off + 4;
        for (uint32_t i = 0; i < ntab && i < 32; i++) {
            if (!in_range(f, p, 14)) break;
            uint32_t sub_len = rd_u16(f, p + 2);
            uint32_t coverage = rd_u16(f, p + 4);
            // Bit 0 set = horizontal; bit 2 set = cross-stream (a
            // vertical shift, not a horizontal one); high byte = format.
            int horizontal = (coverage & 0x0001) != 0;
            int cross = (coverage & 0x0004) != 0;
            int format = (int)(coverage >> 8);
            uint32_t npairs = rd_u16(f, p + 6);
            uint32_t pairs = p + 14;
            if (format == 0 && horizontal && !cross && npairs > 0
                && in_range(f, pairs, npairs * 6)) {
                f->kern_pairs_off = pairs;
                f->kern_pairs = (int)npairs;
                break; // the first usable subtable wins
            }
            if (sub_len < 14) break; // a zero-length subtable would loop forever
            p += sub_len;
        }
    }
    return 1;
}

// --- cmap ------------------------------------------------------------

static int cmap_lookup_f4(const struct ttf_font *f, uint32_t sub, uint32_t cp) {
    if (cp > 0xFFFF) return 0;
    uint32_t segx2 = rd_u16(f, sub + 6);
    if (!segx2 || (segx2 & 1)) return 0;
    uint32_t ends = sub + 14;
    uint32_t starts = ends + segx2 + 2;
    uint32_t deltas = starts + segx2;
    uint32_t ranges = deltas + segx2;

    for (uint32_t i = 0; i < segx2; i += 2) {
        if (cp > rd_u16(f, ends + i)) continue;
        uint32_t start = rd_u16(f, starts + i);
        if (cp < start) return 0; // segments are sorted; past it means absent
        int32_t delta = rd_i16(f, deltas + i);
        uint32_t ro = rd_u16(f, ranges + i);
        if (ro == 0) return (int)((cp + (uint32_t)delta) & 0xFFFF);
        // glyphIdArray is addressed relative to the range offset's OWN
        // slot, which is the one piece of cmap format 4 that reads like
        // a typo and is not.
        uint32_t gi_off = ranges + i + ro + (cp - start) * 2;
        uint32_t gid = rd_u16(f, gi_off);
        if (!gid) return 0;
        return (int)((gid + (uint32_t)delta) & 0xFFFF);
    }
    return 0;
}

static int cmap_lookup_f12(const struct ttf_font *f, uint32_t sub, uint32_t cp) {
    uint32_t ngroups = rd_u32(f, sub + 12);
    if (ngroups > 100000) return 0;
    for (uint32_t i = 0; i < ngroups; i++) {
        uint32_t g = sub + 16 + i * 12;
        uint32_t lo = rd_u32(f, g);
        uint32_t hi = rd_u32(f, g + 4);
        if (cp < lo) return 0;
        if (cp > hi) continue;
        return (int)(rd_u32(f, g + 8) + (cp - lo));
    }
    return 0;
}

int ttf_glyph_index(const struct ttf_font *f, uint32_t codepoint) {
    if (!f->cmap) return 0;
    uint32_t n = rd_u16(f, f->cmap + 2);
    if (n > 64) return 0;

    // Prefer a Unicode subtable, and among those prefer format 12 (full
    // range) over format 4 (BMP). A (3,0) Windows-symbol subtable is
    // taken as a last resort because some monospace faces ship only
    // that -- their codepoints then live at 0xF000 + c, which is why the
    // symbol retry below exists.
    uint32_t best = 0;
    int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rec = f->cmap + 4 + i * 8;
        uint32_t plat = rd_u16(f, rec);
        uint32_t enc = rd_u16(f, rec + 2);
        uint32_t sub = f->cmap + rd_u32(f, rec + 4);
        if (!in_range(f, sub, 4)) continue;
        uint32_t fmt = rd_u16(f, sub);
        if (fmt != 4 && fmt != 12) continue;

        int score = -1;
        if (plat == 3 && enc == 10) score = 4;      // Windows UCS-4
        else if (plat == 0) score = 3;              // Unicode
        else if (plat == 3 && enc == 1) score = 2;   // Windows BMP
        else if (plat == 3 && enc == 0) score = 1;   // Windows symbol
        if (score > best_score) { best_score = score; best = sub; }
    }
    if (best_score < 0) return 0;

    uint32_t fmt = rd_u16(f, best);
    int gid = fmt == 12 ? cmap_lookup_f12(f, best, codepoint)
                        : cmap_lookup_f4(f, best, codepoint);
    if (!gid && best_score == 1 && codepoint < 0x100) {
        gid = fmt == 12 ? cmap_lookup_f12(f, best, 0xF000 + codepoint)
                        : cmap_lookup_f4(f, best, 0xF000 + codepoint);
    }
    if (gid < 0 || gid >= f->num_glyphs) return 0;
    return gid;
}

// --- metrics ---------------------------------------------------------

int ttf_advance_units(const struct ttf_font *f, int gid) {
    if (!f->hmtx || f->num_hmetrics <= 0) return f->units_per_em;
    int i = gid;
    // Past numberOfHMetrics every glyph shares the LAST entry's
    // advance. That is not a fallback -- it is how a monospace font
    // stores one advance for thousands of glyphs, so the common case
    // for the faces this loads goes through here.
    if (i >= f->num_hmetrics) i = f->num_hmetrics - 1;
    if (i < 0) return f->units_per_em;
    return (int)rd_u16(f, f->hmtx + (uint32_t)i * 4);
}

int ttf_advance_px(const struct ttf_font *f, int gid, int px) {
    fx_t scale = ttf_scale_for_px(f, px);
    return fx_round(fx_mul(fx_from_int(ttf_advance_units(f, gid)), scale));
}

// --- kerning ---------------------------------------------------------

int ttf_kern_units(const struct ttf_font *f, int left_gid, int right_gid) {
    if (!f || !f->kern_pairs || left_gid < 0 || right_gid < 0) return 0;
    // The pair array is sorted by the 32-bit key (left << 16) | right,
    // which the spec guarantees and which is what makes this a binary
    // search over 908 entries rather than a scan per character pair.
    // A font that lied about the ordering gets a wrong-or-zero kern,
    // never an out-of-range read: every access below is through rd_u16.
    uint32_t want = ((uint32_t)left_gid << 16) | (uint32_t)right_gid;
    int lo = 0, hi = f->kern_pairs - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t e = f->kern_pairs_off + (uint32_t)mid * 6;
        uint32_t key = ((uint32_t)rd_u16(f, e) << 16) | rd_u16(f, e + 2);
        if (key == want) return (int)rd_i16(f, e + 4);
        if (key < want) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

int ttf_kern_px(const struct ttf_font *f, int left_gid, int right_gid, int px) {
    int units = ttf_kern_units(f, left_gid, right_gid);
    if (!units) return 0;
    return fx_round(fx_mul(fx_from_int(units), ttf_scale_for_px(f, px)));
}

// --- synthetic bold --------------------------------------------------

void ttf_embolden(uint8_t *cov, int w, int h, int strength) {
    if (!cov || w <= 0 || h <= 0) return;
    if (strength < 1) strength = 1;
    if (strength > 4) strength = 4;
    // Right to left, so a column already widened is never re-read as a
    // source -- doing it left to right smears the whole row to full ink
    // instead of thickening the strokes in it.
    for (int y = 0; y < h; y++) {
        uint8_t *row = cov + (int64_t)y * w;
        for (int x = w - 1; x >= 0; x--) {
            uint8_t v = row[x];
            for (int d = 1; d <= strength && x - d >= 0; d++)
                if (row[x - d] > v) v = row[x - d];
            row[x] = v;
        }
    }
}

// --- outline decoding ------------------------------------------------

static int loca_range(const struct ttf_font *f, int gid, uint32_t *start, uint32_t *end) {
    if (gid < 0 || gid >= f->num_glyphs) return 0;
    if (f->loca_long) {
        if (!in_range(f, f->loca + (uint32_t)gid * 4, 8)) return 0;
        *start = rd_u32(f, f->loca + (uint32_t)gid * 4);
        *end = rd_u32(f, f->loca + (uint32_t)gid * 4 + 4);
    } else {
        if (!in_range(f, f->loca + (uint32_t)gid * 2, 4)) return 0;
        *start = rd_u16(f, f->loca + (uint32_t)gid * 2) * 2;
        *end = rd_u16(f, f->loca + (uint32_t)gid * 2 + 2) * 2;
    }
    if (*end < *start) return 0;
    if (*end > f->glyf_len) return 0;
    return 1;
}

// Glyph flags (simple outlines)
#define ON_CURVE      0x01
#define X_SHORT       0x02
#define Y_SHORT       0x04
#define REPEAT_FLAG   0x08
#define X_SAME_OR_POS 0x10
#define Y_SAME_OR_POS 0x20

// Component flags (composite outlines)
#define ARG_1_AND_2_ARE_WORDS 0x0001
#define ARGS_ARE_XY_VALUES    0x0002
#define WE_HAVE_A_SCALE       0x0008
#define MORE_COMPONENTS       0x0020
#define X_AND_Y_SCALE         0x0040
#define TWO_BY_TWO            0x0080

static int decode_outline(const struct ttf_font *f, int gid,
                          int32_t dx_fu, int32_t dy_fu, fx_t scale,
                          fx_t ox, fx_t oy, struct ttf_scratch *sc, int depth);

static int decode_simple(const struct ttf_font *f, uint32_t g, uint32_t gend,
                         int ncont, int32_t dx_fu, int32_t dy_fu, fx_t scale,
                         fx_t ox, fx_t oy, struct ttf_scratch *sc) {
    if (ncont > TTF_MAX_CONTOURS - sc->ncontours) return 0;
    uint32_t p = g + 10;
    int base = sc->npoints;

    int npts = 0;
    for (int i = 0; i < ncont; i++) {
        int e = (int)rd_u16(f, p + (uint32_t)i * 2);
        if (e < npts - 1) return 0; // contour ends must not go backwards
        npts = e + 1;
        if (npts > TTF_MAX_POINTS - base) return 0;
        sc->contour_end[sc->ncontours + i] = base + e;
    }
    p += (uint32_t)ncont * 2;
    uint32_t ilen = rd_u16(f, p);
    p += 2 + ilen; // instructions: this rasterizer does not hint
    if (p > gend) return 0;

    // Flags, with the repeat encoding expanded. In the SCRATCH struct
    // rather than on the stack: TTF_MAX_POINTS bytes is the kernel's
    // entire 1 KiB frame budget on its own.
    uint8_t *flags = sc->flags;
    for (int i = 0; i < npts; ) {
        if (p >= gend) return 0;
        uint8_t fl = (uint8_t)rd_u8(f, p++);
        flags[i++] = fl;
        if (fl & REPEAT_FLAG) {
            uint32_t rep = rd_u8(f, p++);
            while (rep-- && i < npts) flags[i++] = fl;
        }
    }

    // x then y, each a delta chain. The three-way encoding (short and
    // signed by a flag bit, or "same as previous", or a signed short)
    // is why this cannot be a simple loop over fixed-size records.
    int32_t v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & X_SHORT) {
            uint32_t d = rd_u8(f, p++);
            v += (fl & X_SAME_OR_POS) ? (int32_t)d : -(int32_t)d;
        } else if (!(fl & X_SAME_OR_POS)) {
            v += rd_i16(f, p); p += 2;
        }
        sc->px[base + i] = ox + fx_mul(fx_from_int(v + dx_fu), scale);
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & Y_SHORT) {
            uint32_t d = rd_u8(f, p++);
            v += (fl & Y_SAME_OR_POS) ? (int32_t)d : -(int32_t)d;
        } else if (!(fl & Y_SAME_OR_POS)) {
            v += rd_i16(f, p); p += 2;
        }
        // The y flip lives here: font y grows upward from the baseline,
        // a bitmap row index grows downward from the top.
        sc->py[base + i] = oy - fx_mul(fx_from_int(v + dy_fu), scale);
        sc->on[base + i] = (uint8_t)((flags[i] & ON_CURVE) ? 1 : 0);
    }
    if (p > f->len) return 0;

    sc->npoints = base + npts;
    sc->ncontours += ncont;
    return 1;
}

static int decode_composite(const struct ttf_font *f, uint32_t g, uint32_t gend,
                            int32_t dx_fu, int32_t dy_fu, fx_t scale,
                            fx_t ox, fx_t oy, struct ttf_scratch *sc, int depth) {
    uint32_t p = g + 10;
    for (;;) {
        if (p + 4 > gend) return 0;
        uint32_t flags = rd_u16(f, p);
        int comp = (int)rd_u16(f, p + 2);
        p += 4;
        int32_t a1, a2;
        if (flags & ARG_1_AND_2_ARE_WORDS) {
            a1 = rd_i16(f, p); a2 = rd_i16(f, p + 2); p += 4;
        } else {
            a1 = (int8_t)rd_u8(f, p); a2 = (int8_t)rd_u8(f, p + 1); p += 2;
        }
        // A 2x2 transform on a component is SKIPPED, not applied: the
        // Latin coverage this loads uses composites only for accents
        // (a base letter plus a diaeresis, offset), and every one of
        // those is a pure translation. A scaled component therefore
        // renders at its natural size in the right place rather than
        // being refused outright -- wrong, but wrong in a way a reader
        // can see, and the roadmap carries applying it properly.
        if (flags & WE_HAVE_A_SCALE) p += 2;
        else if (flags & X_AND_Y_SCALE) p += 4;
        else if (flags & TWO_BY_TWO) p += 8;

        int32_t cdx = 0, cdy = 0;
        if (flags & ARGS_ARE_XY_VALUES) { cdx = a1; cdy = a2; }
        // Point-matching (the else case) is not implemented: it is
        // vanishingly rare and needs the parent's points already placed.

        if (!decode_outline(f, comp, dx_fu + cdx, dy_fu + cdy, scale, ox, oy, sc, depth + 1))
            return 0;
        if (!(flags & MORE_COMPONENTS)) break;
    }
    return 1;
}

static int decode_outline(const struct ttf_font *f, int gid,
                          int32_t dx_fu, int32_t dy_fu, fx_t scale,
                          fx_t ox, fx_t oy, struct ttf_scratch *sc, int depth) {
    if (depth > 4) return 0; // a composite cycle is a hostile font, not a deep one
    uint32_t start, end;
    if (!loca_range(f, gid, &start, &end)) return 0;
    if (start == end) return 1; // no outline: a space, legitimately

    uint32_t g = f->glyf + start;
    uint32_t gend = f->glyf + end;
    if (!in_range(f, g, 10)) return 0;
    int ncont = (int)rd_i16(f, g);
    if (ncont >= 0)
        return decode_simple(f, g, gend, ncont, dx_fu, dy_fu, scale, ox, oy, sc);
    return decode_composite(f, g, gend, dx_fu, dy_fu, scale, ox, oy, sc, depth);
}

// --- flattening ------------------------------------------------------

static void add_edge(struct ttf_scratch *sc, fx_t x0, fx_t y0, fx_t x1, fx_t y1) {
    if (y0 == y1) return; // a horizontal edge crosses no scanline
    if (sc->nedges >= TTF_MAX_EDGES) return;
    struct ttf_edge *e = &sc->edges[sc->nedges++];
    e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1;
    e->dir = y1 > y0 ? 1 : -1;
}

static fx_t fx_abs(fx_t v) { return v < 0 ? -v : v; }

// Flattens one quadratic into line segments. The count comes from the
// control polygon's length in pixels, so a small glyph costs a few
// segments and a large one gets a smooth curve -- a fixed count is
// either visibly faceted at 24px or wasteful at 8px.
static void add_quad(struct ttf_scratch *sc, fx_t x0, fx_t y0,
                     fx_t cx, fx_t cy, fx_t x1, fx_t y1) {
    fx_t d = fx_abs(cx - x0) + fx_abs(cy - y0) + fx_abs(x1 - cx) + fx_abs(y1 - cy);
    int n = 1 + fx_to_int(d) / 2;
    if (n < 2) n = 2;
    if (n > 24) n = 24;
    fx_t pxp = x0, pyp = y0;
    for (int i = 1; i <= n; i++) {
        fx_t t = fx_div(fx_from_int(i), fx_from_int(n));
        fx_t u = FX_ONE - t;
        // B(t) = u^2*p0 + 2ut*c + t^2*p1
        fx_t uu = fx_mul(u, u), tt = fx_mul(t, t), ut2 = 2 * fx_mul(u, t);
        fx_t nx = fx_mul(uu, x0) + fx_mul(ut2, cx) + fx_mul(tt, x1);
        fx_t ny = fx_mul(uu, y0) + fx_mul(ut2, cy) + fx_mul(tt, y1);
        add_edge(sc, pxp, pyp, nx, ny);
        pxp = nx; pyp = ny;
    }
}

static fx_t mid(fx_t a, fx_t b) { return (a + b) / 2; }

// Walks one contour's points into edges. The TrueType convention this
// implements: consecutive OFF-curve points have an IMPLIED on-curve
// point at their midpoint, and a contour may start off-curve (in which
// case the start is itself an implied midpoint). Getting either wrong
// produces a glyph that is recognisable but subtly wrong, which is
// worse than one that is obviously broken.
static void flatten_contour(struct ttf_scratch *sc, int first, int last) {
    int n = last - first + 1;
    if (n < 2) return;

    fx_t sx, sy;
    int i0;
    if (sc->on[first]) {
        sx = sc->px[first]; sy = sc->py[first]; i0 = first + 1;
    } else if (sc->on[last]) {
        sx = sc->px[last]; sy = sc->py[last]; i0 = first;
    } else {
        sx = mid(sc->px[last], sc->px[first]);
        sy = mid(sc->py[last], sc->py[first]);
        i0 = first;
    }

    fx_t cx = sx, cy = sy;
    for (int k = 0; k < n; k++) {
        int i = first + ((i0 - first) + k) % n;
        if (sc->on[i]) {
            add_edge(sc, cx, cy, sc->px[i], sc->py[i]);
            cx = sc->px[i]; cy = sc->py[i];
        } else {
            int j = first + ((i0 - first) + k + 1) % n;
            fx_t ex, ey;
            if (sc->on[j]) { ex = sc->px[j]; ey = sc->py[j]; k++; }
            else { ex = mid(sc->px[i], sc->px[j]); ey = mid(sc->py[i], sc->py[j]); }
            add_quad(sc, cx, cy, sc->px[i], sc->py[i], ex, ey);
            cx = ex; cy = ey;
        }
    }
    add_edge(sc, cx, cy, sx, sy); // close it: an open contour fills wrongly
}

// --- scanline fill ---------------------------------------------------

// Adds the horizontal coverage of the span [xa, xb) on one sub-scanline
// into the row accumulator, in Q16.16 units of "fraction of a pixel".
static void add_span(struct ttf_scratch *sc, int w, fx_t xa, fx_t xb) {
    if (xb <= xa) return;
    if (xa < 0) xa = 0;
    if (xb > fx_from_int(w)) xb = fx_from_int(w);
    if (xb <= xa) return;

    int pa = fx_to_int(xa);
    int pb = fx_to_int(xb - 1); // last pixel the span touches
    if (pb >= w) pb = w - 1;
    if (pa == pb) {
        sc->acc[pa] += xb - xa;
        return;
    }
    sc->acc[pa] += fx_from_int(pa + 1) - xa;
    for (int p = pa + 1; p < pb; p++) sc->acc[p] += FX_ONE;
    sc->acc[pb] += xb - fx_from_int(pb);
}

int ttf_render_glyph(const struct ttf_font *f, int gid, int px,
                     uint8_t *cov, int w, int h,
                     int x_origin, int baseline,
                     struct ttf_scratch *sc) {
    if (!f || !cov || !sc || w <= 0 || h <= 0 || w > TTF_MAX_CELL_W) return 0;
    fx_t scale = ttf_scale_for_px(f, px);
    if (!scale) return 0;

    sc->npoints = 0;
    sc->ncontours = 0;
    sc->nedges = 0;
    if (!decode_outline(f, gid, 0, 0, scale,
                        fx_from_int(x_origin), fx_from_int(baseline), sc, 0))
        return 0;
    if (sc->npoints == 0) return 1; // a space: nothing to fill

    int first = 0;
    for (int c = 0; c < sc->ncontours; c++) {
        int last = sc->contour_end[c];
        if (last >= sc->npoints) break;
        flatten_contour(sc, first, last);
        first = last + 1;
    }
    if (sc->nedges == 0) return 1;

    for (int row = 0; row < h; row++) {
        for (int i = 0; i < w; i++) sc->acc[i] = 0;
        int any = 0;

        for (int s = 0; s < TTF_SUBSAMPLES; s++) {
            fx_t sy = fx_from_int(row) + (fx_t)((2 * s + 1) * FX_ONE / (2 * TTF_SUBSAMPLES));
            int nx = 0;
            for (int e = 0; e < sc->nedges; e++) {
                const struct ttf_edge *ed = &sc->edges[e];
                fx_t y0 = ed->y0, y1 = ed->y1;
                fx_t lo = y0 < y1 ? y0 : y1;
                fx_t hi = y0 < y1 ? y1 : y0;
                // Half-open in y so a vertex shared by two edges is
                // counted exactly once -- counting it twice puts a
                // one-pixel notch in a diagonal.
                if (sy < lo || sy >= hi) continue;
                fx_t t = fx_div(sy - y0, y1 - y0);
                fx_t x = ed->x0 + fx_mul(t, ed->x1 - ed->x0);
                // Insertion sort as we go: an outline crosses one
                // scanline a handful of times, so this beats sorting
                // afterwards and needs no second pass.
                int k = nx++;
                while (k > 0 && sc->xs[k - 1] > x) {
                    sc->xs[k] = sc->xs[k - 1];
                    sc->xdir[k] = sc->xdir[k - 1];
                    k--;
                }
                sc->xs[k] = x;
                sc->xdir[k] = ed->dir;
                if (nx >= TTF_MAX_EDGES) break;
            }

            // Nonzero winding: inside wherever the accumulated
            // direction is not zero. (TrueType is nonzero, not
            // even-odd -- with even-odd, an 'o' fills solid.)
            int wind = 0;
            fx_t span_start = 0;
            for (int i = 0; i < nx; i++) {
                int prev = wind;
                wind += sc->xdir[i];
                if (prev == 0 && wind != 0) span_start = sc->xs[i];
                else if (prev != 0 && wind == 0) { add_span(sc, w, span_start, sc->xs[i]); any = 1; }
            }
        }
        if (!any) continue;

        uint8_t *out = cov + (size_t)row * (size_t)w;
        for (int i = 0; i < w; i++) {
            if (sc->acc[i] <= 0) continue;
            int32_t a = (sc->acc[i] * 255) / (TTF_SUBSAMPLES * FX_ONE);
            if (a > 255) a = 255;
            if (a > out[i]) out[i] = (uint8_t)a;
        }
    }
    return 1;
}
