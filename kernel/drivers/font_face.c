// The /usr/share/fonts registry: turning a .ttf on disk into an atlas
// gfx.c can draw from. See api/font_face.h for the contract and for why
// the baked font remains the guaranteed fallback.
//
// THREE THINGS HERE ARE NOT OBVIOUS AND ARE LOAD-BEARING.
//
// **The atlas comes from the FRAME allocator, not from kmalloc.**
// WIN_REQ_FONT maps the active atlas read-only into every GUI client,
// and a mapping is page-granular -- so an atlas sharing a page with
// other kernel heap objects would hand every client a read-only window
// onto whatever else happened to be allocated beside it. A page-aligned
// pmm_alloc_contiguous() run contains the atlas and nothing else. (The
// baked tables never had this problem: they are .rodata, and the whole
// page around them is font data too.)
//
// **An atlas is CACHED AND NEVER FREED.** Clients hold long-lived
// read-only mappings of it, and this kernel has no way to ask them to
// let go -- freeing an atlas on a font-size change would hand the
// compositor a window onto reallocated memory, which is exactly the
// class of bug the poison-page fix (978ebf7) exists to prevent. So a
// rebuilt size allocates a new one and the old one stays. The cache is
// bounded instead, by entries and by total bytes, and a build past
// either bound is REFUSED (the current atlas keeps drawing) rather than
// evicting something a client is reading.
//
// **The cell is measured the way tools/genttf.py measures it.** Same
// formula, deliberately: layout everywhere in this OS is font-derived
// from gfx_char_w()/gfx_char_h(), so a runtime face at 14px producing a
// visibly different cell from the baked 14px would reflow every window
// on the machine the moment a font was selected. The two agree to
// within a pixel, so selecting a face changes the letterforms and not
// the layout.
#include "font_face.h"
#include "ttf.h"
#include "font_ttf.h"
#include "fs.h"
#include "heap.h"
#include "pmm.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include <stddef.h>

#define FONT_DIR "/usr/share/fonts"

// A face file bigger than this is refused rather than loaded. DejaVu
// Sans Mono is 335 KB and Liberation Sans 406 KB; a CJK face is
// megabytes, and this kernel would rather say no than spend them.
#define FONT_FILE_MAX (2u * 1024u * 1024u)

// Bounds on the never-freed atlas cache. 16 entries covers "the user
// tried every size once, in both weights"; 4 MiB is the real stop,
// since one atlas at 64px is ~800 KB while one at 14px is 22 KB.
#define ATLAS_CACHE_MAX 16
#define ATLAS_BYTES_MAX (4ull * 1024 * 1024)

// Sizes outside this are refused. The low end is where a rasterized
// glyph stops being a letter; the high end keeps one cell inside
// TTF_MAX_CELL_W and one atlas inside the cache budget.
#define FONT_PX_MIN 6
#define FONT_PX_MAX 64

struct atlas_entry {
    struct font_atlas a;
    int face;            // index into faces[]
    int used;
};

// The cache is keyed by (face, weight, px) now rather than (face, px),
// so the entry bound has to cover both weights of the sizes anyone
// tried. Raised with it, and the byte bound is still the real stop.


static struct font_face_info faces[FONT_FACE_MAX];
static int face_count;

static int active_face = -1;          // index into faces[], -1 = baked font
static uint8_t *face_data;            // the active face's file bytes (kmalloc)
static uint32_t face_data_len;
static struct ttf_font active_ttf;

// The family's bold member, loaded alongside the regular one. NULL when
// the family has none, which is not a failure: font_face_build() then
// SYNTHESIZES bold from the regular outlines.
static uint8_t *bold_data;
static uint32_t bold_data_len;
static struct ttf_font bold_ttf;
static int bold_loaded;

static struct atlas_entry cache[ATLAS_CACHE_MAX];
static int cache_count;
static uint64_t cache_bytes;

// What gfx.c is drawing from, per weight. Two pointers rather than one
// because both weights are live AT THE SAME TIME -- that is the whole
// point of a weight, unlike a size, where the machine is only ever at
// one. A client maps both (WIN_REQ_FONT takes a weight) and picks per
// run of text.
static const struct font_atlas *current[FONT_WEIGHT_COUNT];

// --- the scan --------------------------------------------------------

// fs_list()'s callback carries no user pointer, so the collection point
// is module state. Nothing else may run a scan concurrently; nothing
// else does (font_face_init() runs at boot and from `fonts --rescan`).
static void scan_cb(const char *name, uint32_t size, int is_dir) {
    if (is_dir || face_count >= FONT_FACE_MAX) return;
    int n = (int)k_strlen(name);
    // Extension match, case-sensitively: a font file here is put there
    // by the build, so there is no need to be clever about ".TTF".
    if (n < 5 || k_strcmp(name + n - 4, ".ttf") != 0) return;

    char stem[FONT_FACE_NAME_LEN];
    int len = n - 4;
    if (len > FONT_FACE_NAME_LEN - 1) len = FONT_FACE_NAME_LEN - 1;
    for (int i = 0; i < len; i++) stem[i] = name[i];
    stem[len] = 0;

    // A `-bold` file is not a face -- it is the bold WEIGHT of the face
    // whose name it is a suffix of. It is skipped here and attached in
    // the pass below, which is why the pass exists: fs_list() gives no
    // ordering guarantee, so the bold file may well arrive before the
    // regular one it belongs to and there would be nothing to attach it
    // to yet.
    int slen = (int)k_strlen(FONT_BOLD_SUFFIX);
    if (len > slen && k_strcmp(stem + len - slen, FONT_BOLD_SUFFIX) == 0) return;

    struct font_face_info *f = &faces[face_count];
    k_strlcpy(f->name, stem, sizeof f->name);
    k_snprintf(f->path, sizeof f->path, FONT_DIR "/%s", name);
    f->size = size;
    f->bold_path[0] = 0;
    f->has_bold = 0;
    face_count++;
}

void font_face_init(void) {
    face_count = 0;
    fs_list(FONT_DIR, scan_cb);

    // Second pass: pair each face with its bold member if the file is
    // there. fs_size() rather than another listing, because that is the
    // one question being asked and the answer is a stat.
    for (int i = 0; i < face_count; i++) {
        char path[FONT_FACE_PATH_LEN];
        k_snprintf(path, sizeof path, FONT_DIR "/%s" FONT_BOLD_SUFFIX ".ttf",
                   faces[i].name);
        if (fs_size(path) > 0) {
            k_strlcpy(faces[i].bold_path, path, sizeof faces[i].bold_path);
            faces[i].has_bold = 1;
        }
    }
}

int font_face_count(void) { return face_count; }

int font_face_info(int index, struct font_face_info *out) {
    if (index < 0 || index >= face_count || !out) return 0;
    *out = faces[index];
    return 1;
}

const char *font_face_active(void) {
    return active_face >= 0 ? faces[active_face].name : "";
}

// --- selecting a face ------------------------------------------------

// Loads and validates one file into `t`, returning the kmalloc'd bytes
// or NULL. The caller owns them and must keep them alive as long as `t`
// is used -- ttf_open() copies nothing (see ttf.h).
static uint8_t *load_ttf(const char *path, struct ttf_font *t, uint32_t *out_len) {
    uint64_t size = fs_size(path);
    if (size == 0 || size > FONT_FILE_MAX) {
        klog_printf("font: %s refused -- %u bytes\n", path, (unsigned)size);
        return 0;
    }
    uint8_t *buf = (uint8_t *)kmalloc((uint32_t)size + 1);
    if (!buf) return 0;
    // fs_read_into(), never fs_read(): this is the kernel context, and
    // fs_read()'s shared staging buffer can be pulled out from under a
    // parse by any ring-3 file syscall (see fs.h, and the cursor theme
    // bug that made the case).
    uint32_t got = fs_read_into(path, buf, (uint32_t)size + 1);
    if (got != size) { kfree(buf); return 0; }
    if (!ttf_open(t, buf, got)) {
        klog_printf("font: %s is not a TrueType outline font\n", path);
        kfree(buf);
        return 0;
    }
    *out_len = got;
    return buf;
}

int font_face_select(const char *name) {
    if (!name || !name[0]) {
        active_face = -1;
        current[FONT_WEIGHT_REGULAR] = 0;
        current[FONT_WEIGHT_BOLD] = 0;
        return 1; // back to the baked font, which cannot fail
    }
    int idx = -1;
    for (int i = 0; i < face_count; i++) {
        if (k_strcmp(faces[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0) return 0;
    if (idx == active_face) return 1;

    struct ttf_font t;
    uint32_t got = 0;
    uint8_t *buf = load_ttf(faces[idx].path, &t, &got);
    if (!buf) return 0;

    // THE BOLD MEMBER IS OPTIONAL AND ITS FAILURE IS NOT THE FACE'S.
    // A family with no bold file, or with one that will not parse, still
    // selects -- bold is synthesized from these outlines instead. The
    // alternative (refusing the whole family) would lose a user their
    // regular font because the bold beside it was corrupt.
    struct ttf_font bt;
    uint32_t bgot = 0;
    uint8_t *bbuf = 0;
    if (faces[idx].has_bold) {
        bbuf = load_ttf(faces[idx].bold_path, &bt, &bgot);
        if (!bbuf)
            klog_printf("font: %s will not load -- bold will be synthesized\n",
                        faces[idx].bold_path);
    }

    // Only now is the previous face given up -- a select that fails
    // leaves the machine drawing with what it already had.
    if (face_data) kfree(face_data);
    if (bold_data) kfree(bold_data);
    face_data = buf;
    face_data_len = got;
    active_ttf = t;
    bold_data = bbuf;
    bold_data_len = bgot;
    bold_loaded = bbuf != 0;
    if (bbuf) bold_ttf = bt;
    active_face = idx;
    current[FONT_WEIGHT_REGULAR] = 0; // gfx_set_font_px() rebuilds
    current[FONT_WEIGHT_BOLD] = 0;
    return 1;
}

// --- building an atlas -----------------------------------------------

static const struct font_atlas *cached(int face, int px, enum font_weight w) {
    for (int i = 0; i < cache_count; i++)
        if (cache[i].used && cache[i].face == face && cache[i].a.px == px
            && cache[i].a.weight == (int)w)
            return &cache[i].a;
    return 0;
}

// The codepoint each of the 101 atlas slots holds -- identical to the
// baked font's slot order (font_ttf.h), which is what makes an atlas a
// drop-in for a baked variant everywhere from gfx_draw_char() to a
// ring-3 client's own glyph indexing.
static uint32_t slot_codepoint(int slot) {
    if (slot < FONT_TTF_ASCII_COUNT) return (uint32_t)(32 + slot);
    return font_ttf_extra_codepoints[slot - FONT_TTF_ASCII_COUNT];
}

// How many pixels a synthetic bold smears by, at `px`. Roughly one
// stroke unit: an em/24 at the sizes this draws, clamped so that it is
// always at least 1 (or the "bold" would be identical to the regular
// and the setting would look broken) and never so wide it closes the
// counters of an 'e' at 8px.
static int synth_strength(int px) {
    int s = px / 24;
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    return s;
}

const struct font_atlas *font_face_build(int px, enum font_weight weight) {
    if (active_face < 0 || !face_data) return 0;
    if (px < FONT_PX_MIN || px > FONT_PX_MAX) return 0;
    if (weight < 0 || weight >= FONT_WEIGHT_COUNT) return 0;

    const struct font_atlas *hit = cached(active_face, px, weight);
    if (hit) { current[weight] = hit; return hit; }

    // WHICH OUTLINES, AND WHETHER TO SMEAR THEM. A real bold file wins;
    // without one, the regular outlines are rasterized and thickened.
    // Both produce an atlas of the same shape, so nothing downstream --
    // gfx.c, WIN_REQ_FONT, a client -- can tell them apart or needs to.
    int want_bold = (weight == FONT_WEIGHT_BOLD);
    int synthetic = want_bold && !bold_loaded;
    const struct ttf_font *t = (want_bold && bold_loaded) ? &bold_ttf : &active_ttf;
    int smear = synthetic ? synth_strength(px) : 0;

    fx_t scale = ttf_scale_for_px(t, px);

    // genttf.py's formula, and the comment at the top of this file says
    // why it is repeated here rather than using the font's raw ascent
    // and descent: those fit every glyph with no clipping at all and
    // produce a visibly taller, looser cell than a terminal font has.
    // The cost is the same slight descender/accent clipping every
    // fixed-cell terminal font accepts.
    int baseline = fx_round(fx_mul(fx_mul(fx_from_int(t->ascent), scale), (fx_t)(FX_ONE * 89 / 100)));
    int below = fx_round(fx_mul(fx_mul(fx_from_int(t->descent), scale), (fx_t)(FX_ONE * 60 / 100)));
    int cell_h = baseline + below;
    if (cell_h < 2) return 0;

    // The cell is as wide as the WIDEST advance in the set, so a
    // proportional face still has a fixed cell for the console and for
    // every caller that has not been taught about advances -- they get
    // a monospaced version of a proportional font, which is ugly but
    // correct, rather than overlapping text.
    int count = FONT_TTF_GLYPH_COUNT;
    uint8_t advances[FONT_TTF_GLYPH_COUNT];
    int cell_w = 1, mono = 1, first_adv = -1;
    int gids[FONT_TTF_GLYPH_COUNT];
    for (int s = 0; s < count; s++) {
        gids[s] = ttf_glyph_index(t, slot_codepoint(s));
        int adv = ttf_advance_px(t, gids[s], px);
        if (adv < 0) adv = 0;
        // A SMEARED GLYPH IS WIDER THAN ITS OUTLINE, so its advance has
        // to grow with it or the next character laps onto its last
        // column. A real bold file needs none of this -- its own hmtx
        // already says how wide its letters are.
        if (adv > 0) adv += smear;
        if (adv > 255) adv = 255;
        advances[s] = (uint8_t)adv;
        if (adv > cell_w) cell_w = adv;
        if (first_adv < 0) first_adv = adv;
        else if (adv != first_adv) mono = 0;
    }
    if (cell_w > TTF_MAX_CELL_W) return 0;

    uint64_t glyph_bytes = (uint64_t)count * (uint64_t)cell_w * (uint64_t)cell_h;
    uint64_t kern_bytes = (uint64_t)count * (uint64_t)count;
    uint64_t total = glyph_bytes + (uint64_t)count + kern_bytes;
    uint64_t pages = (total + 4095) / 4096;

    if (cache_count >= ATLAS_CACHE_MAX || cache_bytes + pages * 4096 > ATLAS_BYTES_MAX) {
        klog_printf("font: atlas cache full (%d entries, %uKB) -- keeping current size\n",
                    cache_count, (unsigned)(cache_bytes / 1024));
        return 0;
    }
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        klog_printf("font: no %uKB contiguous for a %dpx atlas\n",
                    (unsigned)(pages * 4), px);
        return 0;
    }
    uint8_t *blob = (uint8_t *)(uintptr_t)phys; // identity-mapped, see pmm.h
    for (uint64_t i = 0; i < pages * 4096; i++) blob[i] = 0;

    struct ttf_scratch *sc = (struct ttf_scratch *)kmalloc(sizeof *sc);
    if (!sc) {
        pmm_free_contiguous(phys, pages);
        return 0;
    }

    int rendered = 0;
    for (int s = 0; s < count; s++) {
        uint8_t *cell = blob + (uint64_t)s * (uint64_t)cell_w * (uint64_t)cell_h;
        if (ttf_render_glyph(t, gids[s], px, cell, cell_w, cell_h, 0, baseline, sc))
            rendered++;
        // Per cell, AFTER rendering it and before the next one -- the
        // cells are contiguous, so emboldening the whole blob in one
        // pass would smear each glyph into the start of the one after
        // it (they share rows in memory, not on screen).
        if (smear) ttf_embolden(cell, cell_w, cell_h, smear);
    }
    kfree(sc);
    for (int s = 0; s < count; s++) blob[glyph_bytes + s] = advances[s];

    // The kern matrix, in SLOT space. Built here once rather than
    // looked up per draw, because a client has the atlas and not the
    // font file -- it has no cmap and no `kern` table, so anything not
    // baked in now is unavailable to it forever.
    int8_t *kern = (int8_t *)(blob + glyph_bytes + count);
    int kern_nonzero = 0;
    if (t->kern_pairs) {
        for (int l = 0; l < count; l++) {
            for (int r = 0; r < count; r++) {
                int k = ttf_kern_px(t, gids[l], gids[r], px);
                // Clamped rather than wrapped: a kern that will not fit
                // in a byte is a font doing something this OS does not
                // draw, and a wrapped value would move a letter the
                // WRONG WAY by a large amount.
                if (k > 127) k = 127;
                if (k < -127) k = -127;
                kern[l * count + r] = (int8_t)k;
                if (k) kern_nonzero++;
            }
        }
    }

    struct atlas_entry *e = &cache[cache_count++];
    e->used = 1;
    e->face = active_face;
    e->a.glyphs = blob;
    e->a.advances = blob + glyph_bytes;
    e->a.kern = kern;
    e->a.cell_w = cell_w;
    e->a.cell_h = cell_h;
    e->a.count = count;
    e->a.px = px;
    e->a.baseline = baseline;
    e->a.monospace = mono;
    e->a.weight = (int)weight;
    e->a.synthetic = synthetic;
    e->a.phys = phys;
    e->a.bytes = pages * 4096;
    cache_bytes += pages * 4096;

    klog_printf("font: %s %s at %dpx -- %dx%d cell, %d/%d glyphs, %d kern pairs, %s, %uKB\n",
                faces[active_face].name,
                want_bold ? (synthetic ? "bold(synth)" : "bold") : "regular",
                px, cell_w, cell_h, rendered, count, kern_nonzero,
                mono ? "monospace" : "proportional", (unsigned)(e->a.bytes / 1024));
    current[weight] = &e->a;
    return current[weight];
}

const struct font_atlas *font_face_atlas_weight(enum font_weight weight) {
    if (weight < 0 || weight >= FONT_WEIGHT_COUNT) return 0;
    return current[weight];
}

const struct font_atlas *font_face_atlas(void) {
    return current[FONT_WEIGHT_REGULAR];
}

uint64_t font_face_atlas_bytes(void) { return cache_bytes; }
