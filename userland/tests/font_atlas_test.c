// THE ATLAS A RING-3 PROCESS BUILDS IS THE ONE THE KERNEL IS DRAWING
// WITH -- byte for byte, glyph for glyph.
//
// This is the check that makes moving TrueType parsing out of ring 0
// safe to attempt. The compositor, every client and the console all
// index ONE atlas blob, so a font service in ring 3 is only viable if
// what it produces is indistinguishable from what the kernel produces
// today. `api/font_atlas.h` is one implementation compiled twice to make
// that true by construction; this asserts it is actually true, across
// the ring boundary, over the real font on the real disk.
//
// WHAT IT COMPARES. `QUERY_FONTGLYPH` reports, per slot, an FNV-1a over
// the cell's coverage bytes plus the metrics -- so this rebuilds the
// atlas here, hashes each cell the same way, and requires all 101 slots
// and every metric to agree. A hash over the WHOLE cell rather than the
// ink box is what makes it a real comparison: two glyphs agreeing on
// where the ink is and disagreeing on its shape still differ.
//
// IT SKIPS WHEN NO FACE IS LOADED. The baked tables were rasterized at
// build time by genttf.py and are not produced by this code path at all,
// so there is nothing to compare and saying so is honest. `fontface
// dejavu-sans-mono` first if this reports a skip.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "ttf.h"
#include "font_atlas.h"
#include "query_abi.h"
#include "font_face.h"  // FONT_WEIGHT_*, FONT_BOLD_SUFFIX
#include "lib/usetting.h"
#include "lib/ufile.h"
#include "lib/utest.h"

#define FONT_DIR "/usr/share/fonts/"

// The kernel's own hash, repeated so the two sides agree on what
// "identical" means. If this drifts from font_query.c's the test goes
// red, which is the right way round.
static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int query_slot(int slot, struct query_fontglyph *out) {
    return sys_query_record(QUERY_FONTGLYPH, (unsigned)slot, out, sizeof *out) == (int)sizeof *out;
}

int main(void) {
    utest_begin("font_atlas_test",
                "the ring-3 atlas is the kernel's, byte for byte", 0);

    struct query_fontglyph k0;
    if (!query_slot(0, &k0)) {
        utest_check(0, "QUERY_FONTGLYPH answers");
        return utest_end();
    }

    if (!(k0.flags & QUERY_FONTGLYPH_FACE)) {
        // Not a failure: the machine is on the baked tables, which this
        // code did not produce. Naming it is what stops a skip reading
        // as a pass.
        return utest_skip("no face loaded -- the baked tables are not built "
                          "by font_atlas.c (try `fontface dejavu-sans-mono`)");
    }

    char face[64] = {0};
    if (usetting_get("system.font_face", face, sizeof face) <= 0 || !face[0]) {
        utest_check(0, "system.font_face is readable");
        return utest_end();
    }
    utest_notef("face=%s px=%u weight=%u", face, k0.px, k0.weight);

    // WHICH FILE THE KERNEL RASTERIZED depends on the weight it is
    // drawing at, and the bold member is a FILENAME rule rather than
    // metadata. A family with no bold file gets its regular outlines
    // smeared, and this has to smear them the same way or every cell
    // differs.
    int want_bold = (k0.weight == FONT_WEIGHT_BOLD);
    int synthesizing = 0;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char path[128];

    if (want_bold) {
        snprintf(path, sizeof path, "%s%s%s.ttf", FONT_DIR, face, FONT_BOLD_SUFFIX);
        if (ufile_slurp(path, 0, &bytes, &len) != UFILE_OK) synthesizing = 1;
    }
    if (!bytes) {
        snprintf(path, sizeof path, "%s%s.ttf", FONT_DIR, face);
        if (ufile_slurp(path, 0, &bytes, &len) != UFILE_OK) {
            utest_check(0, "the active face is readable");
            return utest_end();
        }
    }

    struct ttf_font t;
    utest_check(ttf_open(&t, bytes, len), "the face parses in ring 3");
    if (utest_failed()) return utest_end();

    struct font_atlas_plan plan;
    utest_check(font_atlas_plan(&t, k0.px, (int)k0.weight, synthesizing, &plan),
                "the atlas plan succeeds at the kernel's size");
    if (utest_failed()) return utest_end();

    // THE METRICS FIRST. A cell one pixel taller reflows every window on
    // the machine, so these are named individually rather than folded
    // into the byte comparison below.
    utest_checkf(plan.cell_w == (int)k0.cell_w, "cell_w agrees (%d vs %u)",
                 plan.cell_w, k0.cell_w);
    utest_checkf(plan.cell_h == (int)k0.cell_h, "cell_h agrees (%d vs %u)",
                 plan.cell_h, k0.cell_h);
    utest_checkf(plan.line_h == (int)k0.line_h, "line_h agrees (%d vs %u)",
                 plan.line_h, k0.line_h);
    utest_checkf(plan.baseline == (int)k0.baseline, "baseline agrees (%d vs %u)",
                 plan.baseline, k0.baseline);
    utest_checkf(plan.count == (int)k0.count, "the slot count agrees (%d vs %u)",
                 plan.count, k0.count);

    uint8_t *blob = (uint8_t *)calloc(1, (size_t)plan.total_bytes);
    struct ttf_scratch *sc = (struct ttf_scratch *)malloc(sizeof *sc);
    if (!blob || !sc) {
        utest_check(0, "memory for the atlas");
        return utest_end();
    }
    font_atlas_render(&t, &plan, blob, sc);

    // EVERY SLOT, not a sample. A face whose digits match and whose
    // letters do not is exactly what this exists to catch, and 101
    // comparisons cost nothing.
    int bad = 0, adv_bad = 0, first_bad = -1;
    for (int s = 0; s < plan.count; s++) {
        struct query_fontglyph k;
        if (!query_slot(s, &k)) { bad++; continue; }
        const uint8_t *cell = blob + (size_t)s * (size_t)plan.cell_w
                                    * (size_t)plan.cell_h;
        uint32_t h = fnv1a(cell, (uint32_t)(plan.cell_w * plan.cell_h));
        if (h != k.hash) { if (first_bad < 0) first_bad = s; bad++; }
        if (plan.advances[s] != k.advance) adv_bad++;
    }

    utest_checkf(bad == 0, "every glyph cell hashes identically (%d/%d differ, first %d)",
                 bad, plan.count, first_bad);
    utest_checkf(adv_bad == 0, "every advance agrees (%d/%d differ)",
                 adv_bad, plan.count);

    free(sc);
    free(blob);
    free(bytes);
    return utest_end();
}
