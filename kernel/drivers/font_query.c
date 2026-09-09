// QUERY_FONTGLYPH -- what one glyph of the running font actually is.
//
// A glyph that rasterised to nothing is pixel-identical on screen to a
// space, to a character the font does not carry, and to a font that
// failed to load. Nothing in this kernel could tell those apart, and it
// has cost a hunt: a client read a session-font cell as entirely blank
// while the kernel had logged 101 of 101 glyphs built, and the session
// was destroyed before anything could ask which of the four it was
// (docs/bugs.md). This is the provider that answers it.
//
// **IT REPORTS WHAT gfx.c WOULD DRAW, not what some font file says.**
// The source is resolved exactly as active_atlas() resolves it -- a
// runtime face when one is selected, the baked tables otherwise -- and
// the slot comes from gfx_glyph_index(), which is the mapping
// gfx_draw_char() uses. A provider that reread the .ttf would be
// answering a different question from the one being asked, and would
// agree with the screen only by coincidence.
//
// It lives in kernel/drivers/ beside font_face.c and font_ttf.c rather
// than under kernel/core/, because a provider belongs to whoever owns
// the fact (api/query.h) and the fact here is the font's.
#include "query.h"
#include "font_ttf.h"
#include "gfx.h"
#include "string.h"
#include "initcall.h"

// driver-none: a QUERY provider over the loaded faces

// The set is the same 101 glyphs everywhere -- ASCII 32..126 then six
// Latin-1 extras. A runtime atlas rasterises exactly that set, which is
// what makes one index answer for both sources.
static int fontglyph_count(void) { return FONT_TTF_GLYPH_COUNT; }

// The codepoint a slot draws. The inverse of gfx_glyph_index(), and it
// is written here rather than exported because this is the only caller
// that ever needs to go this way round -- everything else in the kernel
// starts from a character.
static int slot_codepoint(int slot) {
    if (slot < FONT_TTF_ASCII_COUNT) return slot + 32;
    int extra = slot - FONT_TTF_ASCII_COUNT;
    if (extra < FONT_TTF_EXTRA_COUNT) return font_ttf_extra_codepoints[extra];
    return 0;
}

// FNV-1a, 32-bit. Chosen because the ring-3 half has to compute the
// identical value over its own mapping and this is short enough to be
// obviously the same algorithm in both places -- the whole point of the
// hash is that a disagreement is decisive, so an implementation
// difference would be worse than no hash at all.
static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int fontglyph_fill(int index, void *out) {
    if (index < 0 || index >= FONT_TTF_GLYPH_COUNT) return 0;

    struct query_fontglyph *q = out;
    k_memset(q, 0, sizeof *q);

    // Resolved the way gfx.c resolves it, at the weight gfx.c is
    // currently set to -- so `fontface`, `fontsize` and a `gfx_set_bold`
    // in flight all move this together with the screen.
    // **RING 0 HAS NO RUNTIME FACE, so this reports what the CONSOLE
    // draws** -- the tables baked into the image. The DESKTOP's font is
    // /bin/fontd's and is reported by `diag font`; the two genuinely
    // differ now, which is the split rather than a fault.
    //
    // QUERY_FONTGLYPH_FACE is therefore never set here any more. A
    // reader that treated its absence as "no font" would be wrong: it
    // means "the baked one", which is a font.
    const struct font_ttf_variant *fv = &font_ttf_variants[gfx_font_size()];

    const uint8_t *cell;
    int cw, ch;
    {
        // THE BAKED TABLES HAVE NO SEPARATE LINE BOX AND NO ADVANCES.
        // Reported as cell_h and cell_w rather than left at zero: they
        // are the true answers for that font, and a reader comparing a
        // baked machine against a face machine must not have to know
        // which fields go missing on which.
        cw = fv->w;
        ch = fv->h;
        cell = (const uint8_t *)fv->glyphs + (size_t)index * (size_t)cw * (size_t)ch;
        q->line_h   = (uint16_t)ch;
        q->baseline = 0; // the baked tables do not record one
        q->advance  = (uint16_t)cw;
        q->px       = (uint16_t)gfx_font_px();
        q->count    = FONT_TTF_GLYPH_COUNT;
        q->weight   = 0; // the baked tables have ONE weight
    }

    q->slot      = (uint32_t)index;
    q->codepoint = (uint32_t)slot_codepoint(index);
    q->cell_w    = (uint16_t)cw;
    q->cell_h    = (uint16_t)ch;
    q->hash      = fnv1a(cell, (uint32_t)cw * (uint32_t)ch);

    // The ink box and the peak, in one pass. x1/y1 are EXCLUSIVE and
    // stay 0 when nothing was found, which is why max_coverage is the
    // test a reader is told to use: an empty box and a single lit pixel
    // at the origin are otherwise indistinguishable.
    int x0 = cw, y0 = ch, x1 = 0, y1 = 0;
    uint8_t peak = 0;
    for (int y = 0; y < ch; y++) {
        const uint8_t *row = cell + (size_t)y * (size_t)cw;
        for (int x = 0; x < cw; x++) {
            uint8_t v = row[x];
            if (!v) continue;
            if (v > peak) peak = v;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x + 1 > x1) x1 = x + 1;
            if (y + 1 > y1) y1 = y + 1;
        }
    }
    q->max_coverage = peak;
    if (peak) {
        q->ink_x0 = (uint16_t)x0;
        q->ink_y0 = (uint16_t)y0;
        q->ink_x1 = (uint16_t)x1;
        q->ink_y1 = (uint16_t)y1;
    }

    // THE INK MAP IS CLIPPED RATHER THAN REFUSED, and it says so. A
    // record is 256 bytes (QUERY_RECORD_MAX) and a 64px cell does not
    // fit at any bit depth; a reader looking at a glyph that large is
    // better served by a truncated picture plus a flag than by nothing.
    int map_w = cw > QUERY_FONTGLYPH_MAP_W_MAX ? QUERY_FONTGLYPH_MAP_W_MAX : cw;
    if (map_w < 0) map_w = 0;
    int stride = (map_w + 7) / 8;
    int map_h = ch;
    if (stride > 0) {
        int fits = QUERY_FONTGLYPH_INK_MAX / stride;
        if (map_h > fits) map_h = fits;
    } else {
        map_h = 0;
    }
    if (map_w < cw) q->flags |= QUERY_FONTGLYPH_CLIPPED_W;
    if (map_h < ch) q->flags |= QUERY_FONTGLYPH_CLIPPED_H;
    q->map_w = (uint16_t)map_w;
    q->map_h = (uint16_t)map_h;

    for (int y = 0; y < map_h; y++) {
        const uint8_t *row = cell + (size_t)y * (size_t)cw;
        for (int x = 0; x < map_w; x++) {
            if (!row[x]) continue;
            // MSB first within the byte, so the map reads left to right
            // in a hex dump the same way it draws.
            q->ink[y * stride + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
        }
    }
    return 1;
}

static const struct query_provider fontglyph_provider = {
    .cls = QUERY_FONTGLYPH,
    .name = "fontglyph",
    .record_size = sizeof(struct query_fontglyph),
    .flags = QUERY_F_LIST,
    .count = fontglyph_count,
    .fill = fontglyph_fill,
    // NO NAMED FIELDS, for the reason api/query.h gives and kbdtap
    // repeats: an index baked into a name would name a different glyph
    // the moment the font changed, and `fontglyph.advance` cannot mean
    // anything without saying whose.
    .fields = NULL,
    .field_count = 0,
};

void fontglyph_query_init(void) {
    query_register(&fontglyph_provider);
}
INITCALL(fontglyph_query_init, INIT_QUERY);
