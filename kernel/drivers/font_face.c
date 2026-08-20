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

// Bounds on the never-freed atlas cache. 8 entries covers "the user
// tried every size once"; 4 MiB is the real stop, since one atlas at
// 64px is ~800 KB while one at 14px is 22 KB.
#define ATLAS_CACHE_MAX 8
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

static struct font_face_info faces[FONT_FACE_MAX];
static int face_count;

static int active_face = -1;          // index into faces[], -1 = baked font
static uint8_t *face_data;            // the active face's file bytes (kmalloc)
static uint32_t face_data_len;
static struct ttf_font active_ttf;

static struct atlas_entry cache[ATLAS_CACHE_MAX];
static int cache_count;
static uint64_t cache_bytes;

static const struct font_atlas *current;  // what gfx.c is drawing from

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

    struct font_face_info *f = &faces[face_count];
    int stem = n - 4;
    if (stem > FONT_FACE_NAME_LEN - 1) stem = FONT_FACE_NAME_LEN - 1;
    for (int i = 0; i < stem; i++) f->name[i] = name[i];
    f->name[stem] = 0;
    k_snprintf(f->path, sizeof f->path, FONT_DIR "/%s", name);
    f->size = size;
    face_count++;
}

void font_face_init(void) {
    face_count = 0;
    fs_list(FONT_DIR, scan_cb);
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

int font_face_select(const char *name) {
    if (!name || !name[0]) {
        active_face = -1;
        current = 0;
        return 1; // back to the baked font, which cannot fail
    }
    int idx = -1;
    for (int i = 0; i < face_count; i++) {
        if (k_strcmp(faces[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0) return 0;
    if (idx == active_face) return 1;

    uint64_t size = fs_size(faces[idx].path);
    if (size == 0 || size > FONT_FILE_MAX) {
        klog_printf("font: %s refused -- %u bytes\n", faces[idx].path, (unsigned)size);
        return 0;
    }
    uint8_t *buf = (uint8_t *)kmalloc((uint32_t)size + 1);
    if (!buf) return 0;
    // fs_read_into(), never fs_read(): this is the kernel context, and
    // fs_read()'s shared staging buffer can be pulled out from under a
    // parse by any ring-3 file syscall (see fs.h, and the cursor theme
    // bug that made the case).
    uint32_t got = fs_read_into(faces[idx].path, buf, (uint32_t)size + 1);
    if (got != size) { kfree(buf); return 0; }

    struct ttf_font t;
    if (!ttf_open(&t, buf, got)) {
        klog_printf("font: %s is not a TrueType outline font\n", faces[idx].path);
        kfree(buf);
        return 0;
    }

    // Only now is the previous face given up -- a select that fails
    // leaves the machine drawing with what it already had.
    if (face_data) kfree(face_data);
    face_data = buf;
    face_data_len = got;
    active_ttf = t;
    active_face = idx;
    current = 0; // gfx_set_font_px() rebuilds
    return 1;
}

// --- building an atlas -----------------------------------------------

static const struct font_atlas *cached(int face, int px) {
    for (int i = 0; i < cache_count; i++)
        if (cache[i].used && cache[i].face == face && cache[i].a.px == px)
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

const struct font_atlas *font_face_build(int px) {
    if (active_face < 0 || !face_data) return 0;
    if (px < FONT_PX_MIN || px > FONT_PX_MAX) return 0;

    const struct font_atlas *hit = cached(active_face, px);
    if (hit) { current = hit; return hit; }

    const struct ttf_font *t = &active_ttf;
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
        if (adv > 255) adv = 255;
        advances[s] = (uint8_t)adv;
        if (adv > cell_w) cell_w = adv;
        if (first_adv < 0) first_adv = adv;
        else if (adv != first_adv) mono = 0;
    }
    if (cell_w > TTF_MAX_CELL_W) return 0;

    uint64_t glyph_bytes = (uint64_t)count * (uint64_t)cell_w * (uint64_t)cell_h;
    uint64_t total = glyph_bytes + (uint64_t)count;
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
    }
    kfree(sc);
    for (int s = 0; s < count; s++) blob[glyph_bytes + s] = advances[s];

    struct atlas_entry *e = &cache[cache_count++];
    e->used = 1;
    e->face = active_face;
    e->a.glyphs = blob;
    e->a.advances = blob + glyph_bytes;
    e->a.cell_w = cell_w;
    e->a.cell_h = cell_h;
    e->a.count = count;
    e->a.px = px;
    e->a.baseline = baseline;
    e->a.monospace = mono;
    e->a.phys = phys;
    e->a.bytes = pages * 4096;
    cache_bytes += pages * 4096;

    klog_printf("font: %s at %dpx -- %dx%d cell, %d/%d glyphs, %s, %uKB\n",
                faces[active_face].name, px, cell_w, cell_h, rendered, count,
                mono ? "monospace" : "proportional", (unsigned)(e->a.bytes / 1024));
    current = &e->a;
    return current;
}

const struct font_atlas *font_face_atlas(void) { return current; }

uint64_t font_face_atlas_bytes(void) { return cache_bytes; }
