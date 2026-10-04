// The atlas rasteriser: one implementation, compiled into ring 0 and
// ring 3. See api/font_atlas.h for why, and for the ABI the three
// sections make.
//
// SHARED SOURCE: this file may name nothing kernel-only -- no kmalloc,
// no pmm, no fs, no klog. Everything it needs comes in as an argument,
// which is what lets a ring-3 font service produce an atlas the kernel
// can use unchanged.
#include "font_atlas.h"
#include "fixed.h"

uint32_t font_atlas_slot_codepoint(int slot) {
    return font_ttf_slot_codepoint(slot);
}

// How hard to smear a synthesized bold. Scaled with the size, because a
// fixed one-pixel smear is invisible at 32px and turns 8px into a blot.
static int synth_strength(int px) {
    int s = px / 24;
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    return s;
}

// A GLYPH THAT FELL ENTIRELY BELOW THE LINE BOX IS PULLED BACK INTO IT.
// Underscore is the case: its ink sits at or under the baseline, so at
// some sizes the whole thing lands in the descender rows, and a caller
// painting only `line_h` rows draws nothing at all. Collapsing it to a
// single row on the last line of the box is what a terminal font does.
static void lift_into_line_box(uint8_t *cell, int cell_w, int cell_h, int line_h) {
    if (line_h <= 0 || line_h >= cell_h) return;

    int peak = 0, peak_in = 0;
    for (int r = 0; r < cell_h; r++)
        for (int c = 0; c < cell_w; c++) {
            int v = cell[r * cell_w + c];
            if (v > peak) peak = v;
            if (r < line_h && v > peak_in) peak_in = v;
        }
    if (!peak || peak_in) return; // blank, or already visible -- leave it alone

    int thresh = peak / 2;
    uint8_t row[TTF_MAX_CELL_W]; // the bound the plan already enforces
    for (int c = 0; c < cell_w && c < (int)sizeof row; c++) {
        int best = 0;
        for (int r = 0; r < cell_h; r++) {
            int v = cell[r * cell_w + c];
            if (v > best) best = v;
        }
        row[c] = best >= thresh && best > 0 ? 255 : 0;
    }
    for (int r = 0; r < cell_h; r++)
        for (int c = 0; c < cell_w; c++) cell[r * cell_w + c] = 0;
    for (int c = 0; c < cell_w && c < (int)sizeof row; c++)
        cell[(line_h - 1) * cell_w + c] = row[c];
}

int font_atlas_plan(const struct ttf_font *t, int px, int weight,
                    int synthesizing, struct font_atlas_plan *p) {
    if (!t || !p || px <= 0) return 0;

    p->px = px;
    p->weight = weight;
    p->synthetic = synthesizing ? 1 : 0;
    p->smear = synthesizing ? synth_strength(px) : 0;

    fx_t scale = ttf_scale_for_px(t, px);

    // genttf.py's formula, repeated here rather than using the font's
    // raw ascent and descent: those fit every glyph with no clipping and
    // produce a visibly taller, looser cell than a terminal font has.
    // The cost is the slight descender/accent clipping every fixed-cell
    // terminal font accepts.
    int baseline = fx_round(fx_mul(fx_mul(fx_from_int(t->ascent), scale),
                                    (fx_t)(FX_ONE * 89 / 100)));
    int below = fx_round(fx_mul(fx_mul(fx_from_int(t->descent), scale),
                                 (fx_t)(FX_ONE * 60 / 100)));

    // THE LINE PITCH IS THE SQUEEZED HEIGHT, UNCHANGED. Every
    // font-derived measurement on the machine comes from this, so it
    // must stay exactly what genttf.py's formula produced or the whole
    // UI reflows and stops matching the baked tables.
    int line_h = baseline + below;
    if (line_h < 2) return 0;

    // ...WHILE THE BITMAP GETS THE FULL DESCENT, so a 'g' has its tail.
    // Extended DOWNWARD only -- the baseline is untouched, so text sits
    // where it always did. +1 because baseline and descent round
    // independently, so a glyph reaching exactly the descent line would
    // otherwise land on the row after the last.
    int full_below = fx_round(fx_mul(fx_from_int(t->descent), scale));
    if (full_below < below) full_below = below;
    int cell_h = baseline + full_below + 1;

    // The cell is as wide as the WIDEST advance in the set, so a
    // proportional face still has a fixed cell for the console and for
    // every caller that has not been taught about advances -- they get a
    // monospaced version of a proportional font, which is ugly but
    // correct, rather than overlapping text.
    int count = FONT_TTF_GLYPH_COUNT;
    int cell_w = 1, mono = 1, first_adv = -1;
    for (int s = 0; s < count; s++) {
        p->gids[s] = ttf_glyph_index(t, font_ttf_slot_draw_codepoint(s));
        int adv = ttf_advance_px(t, p->gids[s], px);
        if (adv < 0) adv = 0;
        // A SMEARED GLYPH IS WIDER THAN ITS OUTLINE, so its advance has
        // to grow with it or the next character laps onto its last
        // column. A real bold file needs none of this -- its own hmtx
        // already says how wide its letters are.
        if (adv > 0) adv += p->smear;
        if (adv > 255) adv = 255;
        p->advances[s] = (uint8_t)adv;
        if (adv > cell_w) cell_w = adv;
        if (first_adv < 0) first_adv = adv;
        else if (adv != first_adv) mono = 0;
    }
    if (cell_w > TTF_MAX_CELL_W) return 0;

    p->cell_w = cell_w;
    p->cell_h = cell_h;
    p->line_h = line_h;
    p->baseline = baseline;
    p->count = count;
    p->monospace = mono;
    p->glyph_bytes = (uint64_t)count * (uint64_t)cell_w * (uint64_t)cell_h;
    p->total_bytes = p->glyph_bytes + (uint64_t)count
                   + (uint64_t)count * (uint64_t)count;
    return 1;
}

int font_atlas_render(const struct ttf_font *t, const struct font_atlas_plan *p,
                      uint8_t *out, struct ttf_scratch *sc) {
    if (!t || !p || !out || !sc) return 0;

    int count = p->count, cell_w = p->cell_w, cell_h = p->cell_h;
    int rendered = 0;
    for (int s = 0; s < count; s++) {
        uint8_t *cell = out + (uint64_t)s * (uint64_t)cell_w * (uint64_t)cell_h;
        if (ttf_render_glyph(t, p->gids[s], p->px, cell, cell_w, cell_h, 0,
                             p->baseline, sc))
            rendered++;
        // Per cell, AFTER rendering it and before the next one -- the
        // cells are contiguous, so emboldening the whole blob in one
        // pass would smear each glyph into the start of the one after it
        // (they share rows in memory, not on screen).
        if (p->smear) ttf_embolden(cell, cell_w, cell_h, p->smear);
        lift_into_line_box(cell, cell_w, cell_h, p->line_h);
    }

    for (int s = 0; s < count; s++) out[p->glyph_bytes + s] = p->advances[s];

    // The kern matrix, in SLOT space. Built once rather than looked up
    // per draw, because a client has the atlas and not the font file --
    // it has no cmap and no `kern` table, so anything not baked in now is
    // unavailable to it forever.
    int8_t *kern = (int8_t *)(out + p->glyph_bytes + count);
    if (t->kern_pairs) {
        for (int l = 0; l < count; l++) {
            for (int r = 0; r < count; r++) {
                int k = ttf_kern_px(t, p->gids[l], p->gids[r], p->px);
                // Clamped rather than wrapped: a kern that will not fit
                // in a byte is a font doing something this OS does not
                // draw, and a wrapped value would move a letter the
                // WRONG WAY by a large amount.
                if (k > 127) k = 127;
                if (k < -127) k = -127;
                kern[l * count + r] = (int8_t)k;
            }
        }
    }
    return rendered;
}
