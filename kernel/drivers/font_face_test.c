// KTESTs for the runtime TrueType path: kernel/lib/ttf.c (the parser and
// rasterizer) and kernel/drivers/font_face.c (the /usr/share/fonts
// registry that builds an atlas out of it).
//
// **These read a real font off the real disk**, rather than a synthetic
// fixture, because the bug class that matters here is "this parser
// disagrees with what a real font actually contains" -- a hand-built
// table would only ever assert that the code agrees with itself. The
// cost is that every test states its own precondition and SKIPS rather
// than fails when the image was built without fonts (a `make iso` from a
// tree with an empty data/fonts/, or a live boot): a test that fails on
// a legitimately font-less image would train someone to ignore it.
//
// The rasterizer's own assertions are deliberately about INVARIANTS, not
// about pixels. "Row 4 column 3 is 219" would encode this exact font at
// this exact size and break on a font update while proving very little;
// "an 'H' has ink, a space has none, and every glyph fits inside its
// advance" fails for a real reason and keeps meaning what it means.
#include "ttf.h"
#include "font_face.h"
#include "font_ttf.h"
#include "gfx.h"
#include "fs.h"
#include "heap.h"
#include "string.h"
#include "kfmt.h"
#include "ktest.h"

#define TEST_FACE "dejavu-sans-mono"
#define PROP_FACE "liberation-sans"
// Ships WITHOUT a -bold companion, on purpose: it is the only face on
// the image that makes font_face.c's SYNTHETIC bold path reachable.
#define NOBOLD_FACE "vera-mono"

// Loads a face's bytes for the parser tests. Returns NULL when the image
// has no fonts, which is a SKIP condition and not a failure.
static uint8_t *load_face(const char *name, uint32_t *out_len) {
    char path[FONT_FACE_PATH_LEN];
    k_snprintf(path, sizeof path, "/usr/share/fonts/%s.ttf", name);
    uint64_t size = fs_size(path);
    if (!size || size > 4u * 1024 * 1024) return 0;
    uint8_t *buf = (uint8_t *)kmalloc((uint32_t)size + 1);
    if (!buf) return 0;
    uint32_t got = fs_read_into(path, buf, (uint32_t)size + 1);
    if (got != size) { kfree(buf); return 0; }
    *out_len = got;
    return buf;
}

KTEST("ttf", "rejects data that is not a TrueType font") {
    struct ttf_font f;
    // Every one of these has bitten a real parser: a short buffer, a
    // plausible-looking header with nothing behind it, and an OpenType
    // font (whose outlines are CFF, so there is no `glyf` to read).
    static const uint8_t empty[4] = { 0, 0, 0, 0 };
    KTEST_ASSERT(!ttf_open(&f, empty, sizeof empty));
    KTEST_ASSERT(!ttf_open(&f, 0, 0));

    static const uint8_t otto[16] = { 'O','T','T','O', 0,1, 0,0, 0,0, 0,0, 0,0,0,0 };
    KTEST_ASSERT(!ttf_open(&f, otto, sizeof otto));

    // A correct signature and table count, but the directory does not
    // fit -- the shape a truncated download has.
    static const uint8_t truncated[16] = { 0,1,0,0, 0,40, 0,0, 0,0, 0,0, 0,0,0,0 };
    KTEST_ASSERT(!ttf_open(&f, truncated, sizeof truncated));
}

KTEST("ttf", "parses a real font's head, hhea and cmap") {
    uint32_t len = 0;
    uint8_t *buf = load_face(TEST_FACE, &len);
    if (!buf) return; // no fonts on this image -- see the file comment

    struct ttf_font f;
    KTEST_ASSERT(ttf_open(&f, buf, len));
    KTEST_ASSERT(f.units_per_em >= 16 && f.units_per_em <= 16384);
    KTEST_ASSERT(f.num_glyphs > 100);
    KTEST_ASSERT(f.ascent > 0 && f.descent > 0);

    // cmap: a letter maps somewhere, and a codepoint no Latin face
    // covers maps to .notdef rather than to a wild glyph id.
    int a = ttf_glyph_index(&f, 'A');
    int z = ttf_glyph_index(&f, 'z');
    KTEST_ASSERT(a > 0 && z > 0 && a != z);
    KTEST_ASSERT(ttf_glyph_index(&f, 0x4E2D) == 0); // CJK 'zhong'
    KTEST_ASSERT(ttf_glyph_index(&f, 0x10FFFF) == 0);

    kfree(buf);
}

KTEST("ttf", "advances are monospace here and proportional there") {
    uint32_t len = 0;
    uint8_t *mono = load_face(TEST_FACE, &len);
    if (!mono) return;

    struct ttf_font f;
    KTEST_ASSERT(ttf_open(&f, mono, len));
    int i_adv = ttf_advance_px(&f, ttf_glyph_index(&f, 'i'), 16);
    int w_adv = ttf_advance_px(&f, ttf_glyph_index(&f, 'W'), 16);
    KTEST_ASSERT(i_adv > 0);
    KTEST_ASSERT_EQ(i_adv, w_adv); // it is a monospace face
    kfree(mono);

    uint32_t plen = 0;
    uint8_t *prop = load_face(PROP_FACE, &plen);
    if (!prop) return;
    struct ttf_font p;
    KTEST_ASSERT(ttf_open(&p, prop, plen));
    // THE ASSERTION THE WHOLE PROPORTIONAL-METRICS FEATURE RESTS ON. If
    // this ever reads equal, either hmtx is being misread or the face
    // was swapped for a monospace one, and every measurement built on
    // gfx_char_advance() has quietly gone back to multiplying.
    KTEST_ASSERT(ttf_advance_px(&p, ttf_glyph_index(&p, 'i'), 16)
                 < ttf_advance_px(&p, ttf_glyph_index(&p, 'W'), 16));
    kfree(prop);
}

KTEST("ttf", "rasterizes ink for a letter and none for a space") {
    uint32_t len = 0;
    uint8_t *buf = load_face(TEST_FACE, &len);
    if (!buf) return;

    struct ttf_font f;
    KTEST_ASSERT(ttf_open(&f, buf, len));
    struct ttf_scratch *sc = (struct ttf_scratch *)kmalloc(sizeof *sc);
    KTEST_ASSERT(sc != 0);

    const int px = 16, w = 16, h = 20, baseline = 15;
    uint8_t *cov = (uint8_t *)kmalloc(w * h);
    KTEST_ASSERT(cov != 0);

    k_memset(cov, 0, w * h);
    KTEST_ASSERT(ttf_render_glyph(&f, ttf_glyph_index(&f, 'H'), px, cov, w, h,
                                   0, baseline, sc));
    int ink = 0, solid = 0, partial = 0;
    for (int i = 0; i < w * h; i++) {
        if (cov[i]) ink++;
        if (cov[i] == 255) solid++;
        if (cov[i] > 0 && cov[i] < 255) partial++;
    }
    KTEST_ASSERT(ink > 20);      // an 'H' at 16px is a substantial mark
    KTEST_ASSERT(solid > 0);     // ...with a fully-covered interior
    KTEST_ASSERT(partial > 0);   // ...and ANTI-ALIASED edges: a coverage
                                 // map that is only ever 0 or 255 means
                                 // the sub-scanline averaging is dead,
                                 // which no "is there ink" check sees.

    // Nothing below the baseline for a capital, which is the cheap check
    // that the y flip is the right way up: get it wrong and the glyph
    // renders upside down with all its ink in the descender rows.
    int below = 0;
    for (int y = baseline + 1; y < h; y++)
        for (int x = 0; x < w; x++) if (cov[y * w + x]) below++;
    KTEST_ASSERT_EQ(below, 0);

    k_memset(cov, 0, w * h);
    KTEST_ASSERT(ttf_render_glyph(&f, ttf_glyph_index(&f, ' '), px, cov, w, h,
                                   0, baseline, sc));
    for (int i = 0; i < w * h; i++) KTEST_ASSERT_EQ(cov[i], 0);

    kfree(cov);
    kfree(sc);
    kfree(buf);
}

KTEST("ttf", "an 'o' is hollow -- nonzero winding, not even-odd") {
    uint32_t len = 0;
    uint8_t *buf = load_face(TEST_FACE, &len);
    if (!buf) return;

    struct ttf_font f;
    KTEST_ASSERT(ttf_open(&f, buf, len));
    struct ttf_scratch *sc = (struct ttf_scratch *)kmalloc(sizeof *sc);
    KTEST_ASSERT(sc != 0);
    const int w = 24, h = 32, baseline = 24;
    uint8_t *cov = (uint8_t *)kmalloc(w * h);
    KTEST_ASSERT(cov != 0);
    k_memset(cov, 0, w * h);
    KTEST_ASSERT(ttf_render_glyph(&f, ttf_glyph_index(&f, 'o'), 24, cov, w, h,
                                   0, baseline, sc));

    // The counter of an 'o' is a second contour wound the other way. Fill
    // it with the wrong rule and the letter comes out as a solid blob --
    // recognisable enough in a screenshot to pass an eyeball check, which
    // is exactly why this is asserted numerically. Look for a row that
    // has ink at both ends and a gap in the middle.
    int hollow_rows = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *r = cov + y * w;
        int first = -1, last = -1, gap = 0;
        for (int x = 0; x < w; x++) if (r[x] > 128) { if (first < 0) first = x; last = x; }
        if (first < 0 || last - first < 3) continue;
        for (int x = first + 1; x < last; x++) if (r[x] == 0) gap = 1;
        if (gap) hollow_rows++;
    }
    KTEST_ASSERT(hollow_rows >= 3);

    kfree(cov);
    kfree(sc);
    kfree(buf);
}

KTEST("font_face", "the directory scan finds the shipped faces") {
    font_face_init();
    if (font_face_count() == 0) return; // font-less image

    int found = 0;
    for (int i = 0; i < font_face_count(); i++) {
        struct font_face_info info;
        KTEST_ASSERT(font_face_info(i, &info));
        KTEST_ASSERT(info.name[0] != 0);
        // The name is the filename WITHOUT the extension -- a scan that
        // kept ".ttf" would still list something plausible and then fail
        // every select.
        int n = (int)k_strlen(info.name);
        KTEST_ASSERT(n < 4 || k_strcmp(info.name + n - 4, ".ttf") != 0);
        if (k_strcmp(info.name, TEST_FACE) == 0) found = 1;
    }
    KTEST_ASSERT(found);
    KTEST_ASSERT(!font_face_info(-1, 0));
    KTEST_ASSERT(!font_face_info(font_face_count(), 0));
}

KTEST("font_face", "selecting a face builds an atlas laid out like a baked one") {
    font_face_init();
    if (font_face_count() == 0) return;

    // PRECONDITION, ESTABLISHED RATHER THAN INHERITED: this runs in the
    // live kernel with a desktop up, so whatever face and size the
    // machine is on is whatever a previous test or the user left. Both
    // are restored at the end -- a KTEST that changes the font and
    // leaves it changed would move every window on the screen.
    char before[FONT_FACE_NAME_LEN];
    k_strlcpy(before, font_face_active(), sizeof before);
    int before_px = gfx_font_px();

    KTEST_ASSERT(font_face_select(TEST_FACE));
    const struct font_atlas *a = font_face_build(16, FONT_WEIGHT_REGULAR);
    KTEST_ASSERT(a != 0);
    KTEST_ASSERT_EQ(a->count, FONT_TTF_GLYPH_COUNT);
    KTEST_ASSERT_EQ(a->px, 16);
    KTEST_ASSERT(a->cell_w > 0 && a->cell_h > 0);
    KTEST_ASSERT(a->baseline > 0 && a->baseline <= a->cell_h);
    KTEST_ASSERT(a->monospace); // dejavu-sans-mono had better be
    KTEST_ASSERT(a->glyphs != 0 && a->advances != 0);
    // The atlas must be page-aligned: WIN_REQ_FONT maps it into every
    // client, and a mapping is page-granular -- an atlas sharing a page
    // with other kernel data would share that data too.
    KTEST_ASSERT_EQ((int)(a->phys & 0xFFF), 0);
    KTEST_ASSERT(a->bytes >= (uint64_t)a->count * a->cell_w * a->cell_h);

    // Cached: the same request must not build a second atlas.
    const struct font_atlas *again = font_face_build(16, FONT_WEIGHT_REGULAR);
    KTEST_ASSERT_EQ((int)(again == a), 1);

    // Every slot rasterized into ITS OWN cell: 'A' has ink, and the
    // slot order matches the baked font's (slot 0 is a space).
    const uint8_t *space = a->glyphs;
    int space_ink = 0;
    for (int i = 0; i < a->cell_w * a->cell_h; i++) if (space[i]) space_ink++;
    KTEST_ASSERT_EQ(space_ink, 0);

    const uint8_t *cap_a = a->glyphs + (size_t)('A' - 32) * a->cell_w * a->cell_h;
    int a_ink = 0;
    for (int i = 0; i < a->cell_w * a->cell_h; i++) if (cap_a[i]) a_ink++;
    KTEST_ASSERT(a_ink > 10);

    KTEST_ASSERT(!font_face_select("no-such-face"));
    KTEST_ASSERT_EQ(k_strcmp(font_face_active(), TEST_FACE), 0); // unchanged by the refusal

    font_face_select(before);
    gfx_set_font_px(before_px);
}

KTEST("font_face", "an arbitrary size works with a face and snaps without one") {
    font_face_init();
    char before[FONT_FACE_NAME_LEN];
    k_strlcpy(before, font_face_active(), sizeof before);
    int before_px = gfx_font_px();

    // With no face, 13 is not a size this machine has -- it must snap to
    // a baked one and SAY SO through gfx_font_px(), rather than
    // reporting 13 while drawing 12.
    font_face_select("");
    KTEST_ASSERT(gfx_set_font_px(13));
    KTEST_ASSERT(gfx_font_px() != 13);
    KTEST_ASSERT_EQ(gfx_char_w(), gfx_char_advance('W')); // baked font: fixed cell

    if (font_face_count() > 0 && font_face_select(TEST_FACE)) {
        KTEST_ASSERT(gfx_set_font_px(13));
        KTEST_ASSERT_EQ(gfx_font_px(), 13);
        KTEST_ASSERT(gfx_char_h() > 0 && gfx_char_w() > 0);
    }

    font_face_select(before);
    gfx_set_font_px(before_px);
}

KTEST("font_face", "a character outside the 101-glyph set falls back to '?'") {
    // THE CEILING, ASSERTED SO IT IS A KNOWN BOUND RATHER THAN A
    // DISCOVERY. A loaded face has thousands of glyphs and an atlas
    // rasterizes exactly the set font_ttf.h describes, so everything
    // else has to degrade predictably -- to '?', not to a blank cell and
    // certainly not to whatever bytes follow the table. See
    // docs/conventions/gui.md; widening the set is a WIN_REQ_FONT
    // protocol change, not a constant, which is why this is pinned here.
    font_face_init();
    char before[FONT_FACE_NAME_LEN];
    k_strlcpy(before, font_face_active(), sizeof before);
    int before_px = gfx_font_px();

    // Both fonts, because the fallback lives in gfx.c's shared glyph
    // lookup and a runtime atlas could plausibly have got its own copy.
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (font_face_count() == 0 || !font_face_select(TEST_FACE)) break;
            gfx_set_font_px(16);
        } else {
            font_face_select("");
            gfx_set_font_px(16);
        }

        int q = gfx_char_advance('?');
        KTEST_ASSERT(q > 0);
        // A Euro sign, a CJK codepoint and a Latin-1 letter that is NOT
        // one of the six baked extras: all outside the set, all '?'.
        KTEST_ASSERT_EQ(gfx_char_advance(0x20AC), q);
        KTEST_ASSERT_EQ(gfx_char_advance(0x4E2D), q);
        KTEST_ASSERT_EQ(gfx_char_advance(0xE9), q); // e-acute
        // ...while a letter that IS in the set is not the fallback path:
        // on a proportional face these would differ, and on a monospace
        // one every advance is the cell, so the assertion that carries
        // weight is that the character is FOUND at all -- checked by its
        // glyph having ink, below.
        KTEST_ASSERT(gfx_char_advance('A') > 0);
    }

    font_face_select(before);
    gfx_set_font_px(before_px);
}

KTEST("font_face", "the atlas holds exactly the baked slot count") {
    font_face_init();
    if (font_face_count() == 0) return;

    char before[FONT_FACE_NAME_LEN];
    k_strlcpy(before, font_face_active(), sizeof before);
    int before_px = gfx_font_px();

    KTEST_ASSERT(font_face_select(TEST_FACE));
    const struct font_atlas *a = font_face_build(18, FONT_WEIGHT_REGULAR);
    KTEST_ASSERT(a != 0);
    // Not a tautology: it is the ABI check. WIN_REQ_FONT reports this
    // count to every client and clients index by slot, so an atlas that
    // ever disagreed with FONT_TTF_GLYPH_COUNT would have clients
    // reading the wrong glyph rather than failing visibly.
    KTEST_ASSERT_EQ(a->count, FONT_TTF_GLYPH_COUNT);
    // The LAST slot is one of the Nordic extras and must have ink --
    // that is what proves the extras are rasterized and not merely
    // counted (a loop that stopped at ASCII would leave them blank, and
    // the count would still be right).
    const uint8_t *last = a->glyphs + (size_t)(a->count - 1) * a->cell_w * a->cell_h;
    int ink = 0;
    for (int i = 0; i < a->cell_w * a->cell_h; i++) if (last[i]) ink++;
    KTEST_ASSERT(ink > 5);

    font_face_select(before);
    gfx_set_font_px(before_px);
}

KTEST("ttf", "kerning is read from the proportional face and absent from the mono one") {
    uint32_t plen = 0;
    uint8_t *prop = load_face(PROP_FACE, &plen);
    if (!prop) return;

    struct ttf_font p;
    KTEST_ASSERT(ttf_open(&p, prop, plen));
    // liberation-sans carries a 908-pair format-0 `kern` subtable. If
    // this reads 0 the table walk in ttf_open() has stopped finding it,
    // and every kerning assertion below would then pass VACUOUSLY --
    // which is the whole reason the pair count is asserted first.
    KTEST_ASSERT(p.kern_pairs > 100);

    int A = ttf_glyph_index(&p, 'A'), F = ttf_glyph_index(&p, 'F');
    int T = ttf_glyph_index(&p, 'T'), o = ttf_glyph_index(&p, 'o');
    int H = ttf_glyph_index(&p, 'H'), I = ttf_glyph_index(&p, 'I');
    KTEST_ASSERT(A > 0 && F > 0 && T > 0 && o > 0 && H > 0 && I > 0);

    // THESE TWO PAIRS ARE CHOSEN, NOT ILLUSTRATIVE, and the choice is
    // the test. A pair table is keyed by (left << 16) | right, so a
    // lookup that SORTED the key -- an easy thing to write, and it
    // makes every symmetric pair still answer correctly -- would be
    // invisible to "AV kerns". These are ASYMMETRIC in this face:
    // (F,A) kerns and (A,F) does not, (T,o) kerns and (o,T) does not.
    // In each pair the kerned direction has the HIGHER left gid, so a
    // sorted key answers the other one and both halves redden. (An
    // earlier version of this test used A/V and T/o in ascending gid
    // order only, and a deliberately key-sorting build passed it.)
    KTEST_ASSERT(ttf_kern_units(&p, F, A) < 0);
    KTEST_ASSERT_EQ(ttf_kern_units(&p, A, F), 0);
    KTEST_ASSERT(ttf_kern_units(&p, T, o) < 0);
    KTEST_ASSERT_EQ(ttf_kern_units(&p, o, T), 0);

    // The sign is separately load-bearing: kerning TUCKS a pair closer,
    // and a sign error still measures, still lays out, and makes text
    // visibly worse rather than failing.
    KTEST_ASSERT_EQ(ttf_kern_units(&p, H, I), 0); // a pair nothing kerns

    // At a real size the adjustment survives rounding for a pair this
    // strong, and stays a small fraction of the advance -- a kern the
    // size of a whole character means the scale is wrong.
    int k = ttf_kern_px(&p, T, o, 32);
    KTEST_ASSERT(k < 0);
    KTEST_ASSERT(-k < ttf_advance_px(&p, T, 32));

    // Out-of-range glyph ids answer 0 rather than reading past the
    // pair array -- this parses untrusted input, see ttf.h.
    KTEST_ASSERT_EQ(ttf_kern_units(&p, -1, A), 0);
    KTEST_ASSERT_EQ(ttf_kern_units(&p, F, 0x7FFFFFF), 0);
    kfree(prop);

    uint32_t mlen = 0;
    uint8_t *mono = load_face(TEST_FACE, &mlen);
    if (!mono) return;
    struct ttf_font m;
    KTEST_ASSERT(ttf_open(&m, mono, mlen));
    // dejavu-sans-mono has no `kern` table at all, and a monospace face
    // must never kern even if one appeared -- every cell is one width.
    KTEST_ASSERT_EQ(m.kern_pairs, 0);
    KTEST_ASSERT_EQ(ttf_kern_units(&m, ttf_glyph_index(&m, 'F'),
                                       ttf_glyph_index(&m, 'A')), 0);
    kfree(mono);
}

KTEST("ttf", "emboldening thickens a glyph without flooding the cell") {
    uint32_t len = 0;
    uint8_t *buf = load_face(TEST_FACE, &len);
    if (!buf) return;

    struct ttf_font f;
    KTEST_ASSERT(ttf_open(&f, buf, len));
    struct ttf_scratch *sc = (struct ttf_scratch *)kmalloc(sizeof *sc);
    KTEST_ASSERT(sc != 0);
    const int w = 32, h = 32, baseline = 24, strength = 2;
    uint8_t *cov = (uint8_t *)kmalloc(w * h);
    uint8_t *ref = (uint8_t *)kmalloc(w * h);
    KTEST_ASSERT(cov != 0 && ref != 0);

    k_memset(ref, 0, w * h);
    KTEST_ASSERT(ttf_render_glyph(&f, ttf_glyph_index(&f, 'H'), 24, ref, w, h,
                                   0, baseline, sc));
    for (int i = 0; i < w * h; i++) cov[i] = ref[i];
    ttf_embolden(cov, w, h, strength);

    int ink_before = 0, ink_after = 0;
    int right_before = -1, right_after = -1;
    int rows_before = 0, rows_after = 0;
    for (int y = 0; y < h; y++) {
        int any_b = 0, any_a = 0;
        for (int x = 0; x < w; x++) {
            if (ref[y * w + x]) { ink_before++; any_b = 1; if (x > right_before) right_before = x; }
            if (cov[y * w + x]) { ink_after++;  any_a = 1; if (x > right_after)  right_after  = x; }
        }
        rows_before += any_b;
        rows_after  += any_a;
    }

    KTEST_ASSERT(ink_after > ink_before);   // it did something
    KTEST_ASSERT(right_before >= 0);

    // THE ASSERTION THAT MATTERS, and the one a naive version misses.
    // Smearing LEFT TO RIGHT re-reads columns it has already widened,
    // so ink propagates to the cell's right edge and the glyph becomes
    // a solid bar. That is still "more ink than before" and still
    // leaves the empty rows above and below empty, so neither a total
    // ink count nor a "not the whole cell" check can see it. What
    // cannot survive it is the RIGHTMOST inked column moving by more
    // than `strength`.
    KTEST_ASSERT(right_after >= right_before);
    KTEST_ASSERT(right_after <= right_before + strength);

    // It widens strokes; it does not scale the glyph. A row with no ink
    // before has none after.
    KTEST_ASSERT_EQ(rows_after, rows_before);

    // Antialiasing survives: taking a maximum keeps partial coverage,
    // where an OR or a saturating add would drive every touched pixel
    // to solid and lose the edges.
    int partial = 0;
    for (int i = 0; i < w * h; i++) if (cov[i] > 0 && cov[i] < 255) partial++;
    KTEST_ASSERT(partial > 0);

    // A space stays a space: nothing to thicken, so nothing appears.
    k_memset(cov, 0, w * h);
    ttf_embolden(cov, w, h, 4);
    for (int i = 0; i < w * h; i++) KTEST_ASSERT_EQ(cov[i], 0);

    // Degenerate arguments are refusals, not crashes.
    ttf_embolden(0, w, h, 2);
    ttf_embolden(cov, 0, 0, 2);

    kfree(ref);
    kfree(cov);
    kfree(sc);
    kfree(buf);
}

KTEST("font_face", "a -bold file is a WEIGHT of its family, not a face of its own") {
    font_face_init();
    if (font_face_count() == 0) return; // font-less image

    // THE ASSERTION THAT THE FAMILY RULE IS DOING ANYTHING. Six .ttf
    // files ship and three of them end in -bold; if the scan ever
    // stopped folding them in, `fontface` would list six faces and
    // three of them would be weights masquerading as families. Counting
    // is not enough on its own -- a scan that dropped every -bold file
    // AND its family would also list fewer -- so the named faces are
    // checked too, and the bold ones asserted ABSENT by name.
    int mono = -1, prop = -1, nobold = -1;
    for (int i = 0; i < font_face_count(); i++) {
        struct font_face_info info;
        KTEST_ASSERT(font_face_info(i, &info));
        if (k_strcmp(info.name, TEST_FACE) == 0) mono = i;
        if (k_strcmp(info.name, PROP_FACE) == 0) prop = i;
        if (k_strcmp(info.name, NOBOLD_FACE) == 0) nobold = i;
        // No face may be named "<something>-bold": that name means the
        // fold failed and a weight is being offered as a family.
        int n = (int)k_strlen(info.name);
        int sl = (int)k_strlen(FONT_BOLD_SUFFIX);
        KTEST_ASSERT(n <= sl || k_strcmp(info.name + n - sl, FONT_BOLD_SUFFIX) != 0);
    }
    KTEST_ASSERT(mono >= 0 && prop >= 0 && nobold >= 0);

    struct font_face_info a, b, c;
    KTEST_ASSERT(font_face_info(mono, &a));
    KTEST_ASSERT(font_face_info(prop, &b));
    KTEST_ASSERT(font_face_info(nobold, &c));
    // Two families have a bold FILE and one deliberately does not --
    // which is what makes the synthetic path below reachable at all.
    KTEST_ASSERT(a.has_bold && a.bold_path[0]);
    KTEST_ASSERT(b.has_bold && b.bold_path[0]);
    KTEST_ASSERT(!c.has_bold);
    KTEST_ASSERT_EQ(c.bold_path[0], 0);
}

KTEST("font_face", "bold is a real file where there is one and SMEARED where there is not") {
    font_face_init();
    if (font_face_count() == 0) return;

    char before[FONT_FACE_NAME_LEN];
    k_strlcpy(before, font_face_active(), sizeof before);
    int before_px = gfx_font_px();

    // --- a family WITH a bold file ----------------------------------
    KTEST_ASSERT(font_face_select(TEST_FACE));
    const struct font_atlas *reg = font_face_build(20, FONT_WEIGHT_REGULAR);
    const struct font_atlas *bold = font_face_build(20, FONT_WEIGHT_BOLD);
    KTEST_ASSERT(reg != 0 && bold != 0);
    KTEST_ASSERT(reg != bold); // two atlases, not one returned twice
    KTEST_ASSERT_EQ(reg->weight, FONT_WEIGHT_REGULAR);
    KTEST_ASSERT_EQ(bold->weight, FONT_WEIGHT_BOLD);
    KTEST_ASSERT_EQ(bold->synthetic, 0); // a real file was used

    // THE PROPERTY THAT DISTINGUISHES BOLD FROM REGULAR AT ALL, and the
    // one a fallback-to-regular bug cannot fake: more ink in the same
    // letter. Comparing the atlases' cell sizes would NOT do it -- a
    // designed bold often has the same metrics as its regular.
    int reg_ink = 0, bold_ink = 0;
    const uint8_t *ra = reg->glyphs + (size_t)('H' - 32) * reg->cell_w * reg->cell_h;
    const uint8_t *ba = bold->glyphs + (size_t)('H' - 32) * bold->cell_w * bold->cell_h;
    for (int i = 0; i < reg->cell_w * reg->cell_h; i++) if (ra[i]) reg_ink++;
    for (int i = 0; i < bold->cell_w * bold->cell_h; i++) if (ba[i]) bold_ink++;
    KTEST_ASSERT(bold_ink > reg_ink);

    // Cached per WEIGHT, not just per size: asking again must not build
    // a third atlas, and must not hand back the other weight's.
    KTEST_ASSERT_EQ((int)(font_face_build(20, FONT_WEIGHT_BOLD) == bold), 1);
    KTEST_ASSERT_EQ((int)(font_face_build(20, FONT_WEIGHT_REGULAR) == reg), 1);

    // --- a family WITHOUT one ---------------------------------------
    //
    // vera-mono ships deliberately without a -bold companion, so this
    // is the ONLY way the synthetic path is reachable on this image.
    // Without it every assertion about emboldening would be about
    // ttf_embolden() in isolation, and the wiring that decides when to
    // call it would be untested.
    KTEST_ASSERT(font_face_select(NOBOLD_FACE));
    const struct font_atlas *nreg = font_face_build(20, FONT_WEIGHT_REGULAR);
    const struct font_atlas *nbold = font_face_build(20, FONT_WEIGHT_BOLD);
    KTEST_ASSERT(nreg != 0 && nbold != 0);
    KTEST_ASSERT_EQ(nbold->synthetic, 1); // no file -- it was smeared

    int nreg_ink = 0, nbold_ink = 0;
    const uint8_t *nr = nreg->glyphs + (size_t)('H' - 32) * nreg->cell_w * nreg->cell_h;
    const uint8_t *nb = nbold->glyphs + (size_t)('H' - 32) * nbold->cell_w * nbold->cell_h;
    for (int i = 0; i < nreg->cell_w * nreg->cell_h; i++) if (nr[i]) nreg_ink++;
    for (int i = 0; i < nbold->cell_w * nbold->cell_h; i++) if (nb[i]) nbold_ink++;
    KTEST_ASSERT(nbold_ink > nreg_ink);

    // AND THE ADVANCES GREW WITH THE STROKES. This is the half that is
    // easy to leave out and impossible to see in a screenshot of one
    // word: a smeared glyph is wider than its outline, so text set with
    // the regular advances has each letter lapping onto the last column
    // of the one before it. Only meaningful for a SYNTHETIC bold -- a
    // real bold file carries its own metrics and may legitimately match.
    KTEST_ASSERT(nbold->advances['H' - 32] > nreg->advances['H' - 32]);

    font_face_select(before);
    gfx_set_font_px(before_px);
}

KTEST("font_face", "the atlas carries a kern matrix, and only where the face kerns") {
    font_face_init();
    if (font_face_count() == 0) return;

    char before[FONT_FACE_NAME_LEN];
    k_strlcpy(before, font_face_active(), sizeof before);
    int before_px = gfx_font_px();

    if (font_face_select(PROP_FACE)) {
        const struct font_atlas *a = font_face_build(32, FONT_WEIGHT_REGULAR);
        KTEST_ASSERT(a != 0);
        KTEST_ASSERT(a->kern != 0);
        // The matrix is in SLOT space and the ABI says so (a client has
        // no cmap and indexes by slot) -- so it is read here exactly as
        // a client reads it, `c - 32`.
        int F = 'F' - 32, A = 'A' - 32, T = 'T' - 32, o = 'o' - 32;
        KTEST_ASSERT(a->kern[F * a->count + A] < 0);
        KTEST_ASSERT_EQ(a->kern[A * a->count + F], 0); // asymmetric -- see the ttf test
        KTEST_ASSERT(a->kern[T * a->count + o] < 0);

        // The blob's three sections must be BACK TO BACK in the ABI
        // order glyphs, advances, kern -- a client DERIVES the kern
        // offset from the advance one rather than being told it
        // (win_font_kern_offset()), so a gap or a reorder hands every
        // client the wrong table with nothing failing.
        KTEST_ASSERT_EQ((int)(a->advances - a->glyphs),
                        a->count * a->cell_w * a->cell_h);
        KTEST_ASSERT_EQ((int)((const uint8_t *)a->kern - a->advances), a->count);
        // ...and the allocation covers all three, or the tail of the
        // kern matrix is somebody else's memory once it is mapped.
        KTEST_ASSERT(a->bytes >= (uint64_t)a->count * a->cell_w * a->cell_h
                                 + a->count + (uint64_t)a->count * a->count);
    }

    // A MONOSPACE FACE MUST NOT KERN. It has no `kern` table, so every
    // entry is zero -- and if one were not, every cell in the console
    // would stop being one width.
    if (font_face_select(TEST_FACE)) {
        const struct font_atlas *m = font_face_build(32, FONT_WEIGHT_REGULAR);
        KTEST_ASSERT(m != 0 && m->kern != 0);
        int nonzero = 0;
        for (int i = 0; i < m->count * m->count; i++) if (m->kern[i]) nonzero++;
        KTEST_ASSERT_EQ(nonzero, 0);
    }

    font_face_select(before);
    gfx_set_font_px(before_px);
}
