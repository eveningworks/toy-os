// ugfx -- see ugfx.h for what this is and why the font arrives the way
// it does.
#include "ui/ugfx.h"
#include "syscall_abi.h"
#include "rt/sys.h" // sys_sbrk, sys_win_request -- the screen half, below
#include "ttf.h"    // the SAME rasterizer the kernel uses -- see ugfx_font_load
#include <stdlib.h>

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

// THE SESSION FONTS, filled by ugfx_font_init(): the desktop's active
// face in each weight, mapped read-only by the server. Zero until then,
// which makes a forgotten init draw nothing rather than dereference a
// wild pointer -- see ugfx.h.
//
// TWO OF THEM AND NOT ONE, because both weights are live at the same
// time. That is the difference between a weight and a size: the machine
// is only ever at one size, so a size change re-maps in place, while a
// widget picks a weight per run of text and needs both there at once.
static struct ugfx_font g_session[UGFX_FONT_WEIGHTS];

// What text draws with right now -- always one of the above, or a font
// a client rasterized for itself (ugfx_font_load). Never NULL after a
// successful init.
static const struct ugfx_font *g_font = &g_session[UGFX_FONT_REGULAR];

// Has the bold weight been asked for since the last ugfx_font_init()?
// See ugfx_font_session(): mapping it eagerly cost every client a
// startup round trip for a weight most never use.
static int g_bold_mapped;

// Kept as the old module-level names so that everything below reads as
// it did; they now just track g_font.
#define g_glyphs      (g_font->glyphs)
#define g_advances    (g_font->advances)
#define g_char_w      (g_font->char_w)
// The BITMAP height -- glyph indexing and the drawing loop. Layout goes
// through ugfx_char_h(), which returns the line pitch instead.
#define g_char_h      (g_font->char_h)
#define g_glyph_count (g_font->count)

struct ugfx_surface ugfx_surface_for_window(uint32_t window, int w, int h) {
    struct ugfx_surface s;
    for (unsigned i = 0; i < sizeof s; i++) ((uint8_t *)&s)[i] = 0;
    s.pixels = (uint32_t *)(uintptr_t)win_buffer_vaddr(window);
    s.w = w;
    s.h = h;
    return s;
}

// --- clip and damage --------------------------------------------------

void ugfx_set_clip_rect(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (!s) return;
    // A non-positive extent is an EMPTY clip, not an absent one. See
    // ugfx.h -- the kernel's equivalent shipped the opposite meaning
    // once and it cost a whole frame drawn outside its damage region.
    if (w <= 0 || h <= 0) {
        s->clip_x0 = s->clip_y0 = s->clip_x1 = s->clip_y1 = 0;
        s->clip_active = 1;
        return;
    }
    s->clip_x0 = x;     s->clip_y0 = y;
    s->clip_x1 = x + w; s->clip_y1 = y + h;
    s->clip_active = 1;
}

void ugfx_clear_clip_rect(struct ugfx_surface *s) {
    if (s) s->clip_active = 0;
}

static inline void dirty_mark(struct ugfx_surface *s, int x, int y) {
    if (s->dirty_x1 <= s->dirty_x0) { // was empty
        s->dirty_x0 = x; s->dirty_x1 = x + 1;
        s->dirty_y0 = y; s->dirty_y1 = y + 1;
        return;
    }
    if (x < s->dirty_x0) s->dirty_x0 = x;
    if (x + 1 > s->dirty_x1) s->dirty_x1 = x + 1;
    if (y < s->dirty_y0) s->dirty_y0 = y;
    if (y + 1 > s->dirty_y1) s->dirty_y1 = y + 1;
}

// For the primitives that write a whole rectangle without going through
// ugfx_put_pixel -- marking the two corners is the same bounding box at
// a fraction of the cost.
static inline void dirty_mark_rect(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    dirty_mark(s, x, y);
    dirty_mark(s, x + w - 1, y + h - 1);
}

int ugfx_damage(const struct ugfx_surface *s, int *x, int *y, int *w, int *h) {
    if (!s || s->dirty_x1 <= s->dirty_x0) return 0;
    if (x) *x = s->dirty_x0;
    if (y) *y = s->dirty_y0;
    if (w) *w = s->dirty_x1 - s->dirty_x0;
    if (h) *h = s->dirty_y1 - s->dirty_y0;
    return 1;
}

void ugfx_damage_reset(struct ugfx_surface *s) {
    if (!s) return;
    s->dirty_x0 = s->dirty_y0 = s->dirty_x1 = s->dirty_y1 = 0;
}

void ugfx_put_pixel(struct ugfx_surface *s, int x, int y, uint32_t color) {
    if (!s || !s->pixels) return;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if (s->clip_active &&
        (x < s->clip_x0 || x >= s->clip_x1 || y < s->clip_y0 || y >= s->clip_y1)) return;
    s->pixels[(uint32_t)y * (uint32_t)s->w + (uint32_t)x] = color;
    dirty_mark(s, x, y);
}

uint32_t ugfx_get_pixel(const struct ugfx_surface *s, int x, int y) {
    if (!s || !s->pixels) return 0;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return s->pixels[(uint32_t)y * (uint32_t)s->w + (uint32_t)x];
}

// Intersects a rectangle with the surface bounds and the active clip.
// Returns 0 if nothing survives. Rectangles are clipped as rectangles
// rather than per pixel -- an intersection is exact here, so the row
// loops below stay whole-row writes instead of becoming a per-pixel
// predicate the way the kernel's gfx_fill_rect() is.
static int clip_rect(const struct ugfx_surface *s, int *x, int *y, int *w, int *h) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->clip_active) {
        if (x0 < s->clip_x0) x0 = s->clip_x0;
        if (y0 < s->clip_y0) y0 = s->clip_y0;
        if (x1 > s->clip_x1) x1 = s->clip_x1;
        if (y1 > s->clip_y1) y1 = s->clip_y1;
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->pixels) return;
    // Clip rather than trust the caller: a client drawing outside its
    // own buffer would be corrupting whatever the allocator put after
    // it, and a bounds mistake is far easier to make in a client than
    // in the WM (the client computes its own layout with no help).
    if (!clip_rect(s, &x, &y, &w, &h)) return;

    for (int j = 0; j < h; j++) {
        uint32_t *row = s->pixels + (uint32_t)(y + j) * (uint32_t)s->w + (uint32_t)x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
    dirty_mark_rect(s, x, y, w, h);
}

void ugfx_blit(struct ugfx_surface *s, int x, int y, int w, int h,
                const uint32_t *src, int src_pitch_px) {
    if (!s || !s->pixels || !src || w <= 0 || h <= 0) return;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clip_rect(s, &dx, &dy, &dw, &dh)) return;
    // How far into the source the surviving rectangle starts -- the clip
    // may have taken rows off the top or columns off the left, and
    // reading from the source's origin regardless is how a clipped blit
    // draws the right pixels in the wrong place.
    int sx = dx - x, sy = dy - y;

    for (int j = 0; j < dh; j++) {
        const uint32_t *srow = src + (uint32_t)(sy + j) * (uint32_t)src_pitch_px + (uint32_t)sx;
        uint32_t *drow = s->pixels + (uint32_t)(dy + j) * (uint32_t)s->w + (uint32_t)dx;
        for (int i = 0; i < dw; i++) drow[i] = srow[i];
    }
    dirty_mark_rect(s, dx, dy, dw, dh);
}

void ugfx_blit_alpha(struct ugfx_surface *s, int x, int y, int w, int h,
                      const uint32_t *src, int src_pitch_px) {
    if (!s || !s->pixels || !src || w <= 0 || h <= 0) return;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clip_rect(s, &dx, &dy, &dw, &dh)) return;
    int sx = dx - x, sy = dy - y;

    for (int j = 0; j < dh; j++) {
        const uint32_t *srow = src + (uint32_t)(sy + j) * (uint32_t)src_pitch_px + (uint32_t)sx;
        uint32_t *drow = s->pixels + (uint32_t)(dy + j) * (uint32_t)s->w + (uint32_t)dx;
        for (int i = 0; i < dw; i++) {
            uint32_t sp = srow[i];
            unsigned a = sp >> 24;
            if (a == 0) continue;                 // nothing to draw
            if (a == 255) { drow[i] = sp & 0x00FFFFFF; continue; }
            uint32_t dp = drow[i];
            uint32_t out = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                unsigned sc = (sp >> shift) & 0xFF;
                unsigned dc = (dp >> shift) & 0xFF;
                unsigned v = (sc * a + dc * (255 - a) + 127) / 255;
                out |= (v & 0xFF) << shift;
            }
            drow[i] = out;
        }
    }
    dirty_mark_rect(s, dx, dy, dw, dh);
}

void ugfx_fill(struct ugfx_surface *s, uint32_t color) {
    if (!s) return;
    ugfx_fill_rect(s, 0, 0, s->w, s->h, color);
}

void ugfx_draw_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    ugfx_fill_rect(s, x, y, w, 1, color);
    ugfx_fill_rect(s, x, y + h - 1, w, 1, color);
    ugfx_fill_rect(s, x, y, 1, h, color);
    ugfx_fill_rect(s, x + w - 1, y, 1, h, color);
}

// Character -> glyph slot. The shared table is ASCII 32..126 laid out
// contiguously from index 0; anything outside that draws as a space,
// which is the quiet-degradation choice (a client rendering a stray
// byte should look wrong, not read out of bounds).
static int glyph_index(unsigned char c) {
    int idx = (int)c - WIN_FONT_FIRST_CHAR;
    if (idx < 0 || idx >= g_glyph_count) return 0;
    return idx;
}

// Asks the server for one weight and fills `out`. Returns 1 on success.
static int map_session_font(int weight, struct ugfx_font *out) {
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_FONT;
    req.window = (uint32_t)weight; // `window` IS the weight here -- see win_proto.h
    if (syscall2(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)&req, 0) != 1) return 0;

    uint64_t base = win_font_vaddr(weight);
    out->char_w = req.a;
    out->char_h = req.b;              // the BITMAP height -- the stride
    out->line_h = (int)req.window;    // ...and the LINE PITCH, separately
    if (out->line_h <= 0) out->line_h = req.b;
    out->count  = req.c;
    // Glyph 0 sits at the mapping's base PLUS the data's offset within
    // its first page -- the tables are ordinary kernel .rodata and do
    // not start on a page boundary. Ignoring `d` here would shift every
    // glyph by a few bytes and render convincing-looking garbage.
    out->glyphs = (const unsigned char *)(uintptr_t)(base + (uint32_t)req.d);
    // `mods` is the advance table's offset in the same mapping, 0 when
    // the font has none. A proportional face loaded from
    // /usr/share/fonts has one; the baked tables never do.
    out->advances = req.mods
        ? (const unsigned char *)(uintptr_t)(base + (uint32_t)req.mods)
        : 0;
    // DERIVED, not returned -- the atlas lays glyphs, advances and kern
    // back to back in that order, so the third starts where the second
    // ends. See win_font_kern_offset() in win_proto.h, which is the one
    // place that arithmetic is written down.
    uint64_t koff = win_font_kern_offset(req.mods, req.c);
    out->kern = koff ? (const signed char *)(uintptr_t)(base + koff) : 0;
    return 1;
}

int ugfx_font_init(void) {
    if (!map_session_font(UGFX_FONT_REGULAR, &g_session[UGFX_FONT_REGULAR]))
        return 0;
    // **BOLD IS MAPPED LAZILY, ON FIRST USE.** Mapping it here cost every
    // client a second WIN_REQ_FONT round trip at startup for a weight
    // most of them never draw -- and that delay was measurable: it
    // pushed screen_surface_test.py's client past the window its first
    // log line was expected in, 2 runs in 4, against 0 in 4 without it.
    //
    // A startup cost paid by every client for a feature used by a few is
    // the wrong trade even when it is small, and it is the shape that
    // gets blamed on something else when it finally matters.
    g_session[UGFX_FONT_BOLD] = g_session[UGFX_FONT_REGULAR];
    g_bold_mapped = 0;

    // A re-init (WIN_EV_FONT) must not leave the current font pointing
    // at a private atlas whose backing the app may have freed, so this
    // resets to the session's regular weight -- which is also what an
    // app expects after the desktop's font changed under it.
    g_font = &g_session[UGFX_FONT_REGULAR];
    return 1;
}

const struct ugfx_font *ugfx_font_session(int weight) {
    if (weight < 0 || weight >= UGFX_FONT_WEIGHTS)
        weight = UGFX_FONT_REGULAR;
    if (weight == UGFX_FONT_BOLD && !g_bold_mapped) {
        // ONCE, and once per font change (ugfx_font_init clears the
        // flag). A failure leaves the slot as the copy of regular that
        // ugfx_font_init put there, so this always returns something
        // drawable -- the baked font has ONE weight, so a machine with
        // no face loaded has no bold at all and an app must not lose its
        // text over that. A widget asking for bold quietly gets regular,
        // the same degradation GDI makes for a family with no bold.
        g_bold_mapped = 1; // set FIRST: a failed map must not retry per draw
        map_session_font(UGFX_FONT_BOLD, &g_session[UGFX_FONT_BOLD]);
    }
    return &g_session[weight];
}

const struct ugfx_font *ugfx_font_current(void) { return g_font; }

const struct ugfx_font *ugfx_set_font(const struct ugfx_font *f) {
    const struct ugfx_font *prev = g_font;
    // NULL means "back to the session's regular weight" rather than
    // "no font": every measurement below would return 0 with a NULL
    // font, and a widget that forgot to restore would collapse the
    // layout of everything drawn after it instead of looking wrong.
    g_font = (f && f->glyphs) ? f : &g_session[UGFX_FONT_REGULAR];
    return prev;
}

// --- a private font, rasterized by this app (tier 2) ------------------
//
// **ONLY THE 95 ASCII SLOTS, and that is a real limit, not laziness.**
// The session atlas carries 101 slots -- ASCII 32..126 plus six Nordic
// letters -- but glyph_index() above, which is the whole of what ring 3
// knows about slot layout, maps `c - WIN_FONT_FIRST_CHAR` and rejects
// anything past `count`. So a client cannot ADDRESS slots 95..100 today
// whatever is in them, and rasterizing them here would produce glyphs
// nothing can ask for. Doing it properly means the UTF-8 migration
// (docs/roadmap.md), which replaces byte-indexed slots outright.
//
// Stated rather than silently matched, because the alternative was to
// copy font_ttf.h's extra-codepoint table into ring 3 -- a second copy
// of a table the kernel already owns, kept true by somebody
// remembering, which is the exact shape this project deletes.
#define PRIV_SLOTS 95

unsigned long ugfx_font_arena_size(int px) {
    if (px < 1) px = 1;
    // A DELIBERATELY LOOSE BOUND. A Latin glyph's advance is well under
    // 2 * px and its cell well under 2 * px tall, so this is roughly 4x
    // what a real face needs -- and that costs almost nothing, because
    // sys_sbrk RESERVES address space and pages arrive on touch (see
    // docs/conventions/kernel.md). The untouched remainder of an
    // over-sized arena is never backed by a frame. Asking the caller to
    // allocate exactly enough would mean measuring the face first, i.e.
    // opening the file twice.
    unsigned long cell = (unsigned long)px * 2;
    return (unsigned long)PRIV_SLOTS * cell * cell
         + PRIV_SLOTS                        // advances
         + (unsigned long)PRIV_SLOTS * PRIV_SLOTS; // kern
}

int ugfx_font_load(const char *path, int px, int bold,
                    struct ugfx_font *f, void *arena, unsigned long arena_size) {
    if (!path || !f || !arena || px < 6 || px > 64) return 0;
    if (arena_size < ugfx_font_arena_size(px)) return 0;

    // The FILE BYTES and the ~69 KB of scratch are transient and come
    // from the heap, not from the caller's arena: ttf_open() copies
    // nothing, so the bytes must outlive the parse but NOT the font --
    // once every glyph is rasterized into the arena, the atlas is
    // self-contained and the file can go. Making the caller hold half a
    // megabyte of .ttf forever, for a font it has already rendered,
    // would be the wrong contract.
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) return 0;
    unsigned long size = (unsigned long)st.size;
    if (size == 0 || size > 4ul * 1024 * 1024) return 0;

    unsigned char *file = (unsigned char *)malloc(size);
    if (!file) return 0;
    int fd = sys_open(path, 0);
    if (fd < 0) { free(file); return 0; }
    // Read in a LOOP: sys_read() is allowed to return short, and a
    // single call that happened to fill a whole font on the shipped
    // filesystem would be a latent bug on any other one.
    unsigned long got = 0;
    while (got < size) {
        int64_t n = sys_read(fd, file + got, size - got);
        if (n <= 0) break;
        got += (unsigned long)n;
    }
    sys_close(fd);
    if (got != size) { free(file); return 0; }

    struct ttf_font t;
    if (!ttf_open(&t, file, (uint32_t)size)) { free(file); return 0; }

    // struct ttf_scratch is ~69 KB -- FAR past the 16 KiB ring-3 stack,
    // and USERLAND_CFLAGS carries a -Wframe-larger-than that would catch
    // it at build time. On the heap, always.
    struct ttf_scratch *sc = (struct ttf_scratch *)malloc(sizeof *sc);
    if (!sc) { free(file); return 0; }

    // **THE FULL ASCENT AND DESCENT, UNLIKE THE SESSION FONT**, and the
    // difference is the point rather than an inconsistency.
    //
    // font_face.c squeezes its cell to ascent*0.89 + descent*0.60,
    // copying tools/genttf.py, and it is right to: the session font's
    // cell IS the layout grid. gfx_char_h() sets console rows, window
    // chrome and every font-derived measurement on the machine, so a
    // looser cell makes the whole UI taller and stops matching the baked
    // metrics. The price is the slight descender clipping every
    // fixed-cell terminal font accepts.
    //
    // A private font is not a grid. It is a run of text one app draws,
    // measured by nothing else, so there is nothing to be tight for --
    // and paying that price here buys nothing and costs a visibly flat
    // 'g'. At 24px in liberation-sans the squeeze removes 2.03px of a
    // 5.09px descender, which is most of the tail.
    //
    // +1 row of slack because baseline and below are rounded
    // independently, so a glyph reaching exactly the descent line can
    // otherwise land on the cell's last row.
    fx_t scale = ttf_scale_for_px(&t, px);
    int baseline = fx_round(fx_mul(fx_from_int(t.ascent), scale));
    int below = fx_round(fx_mul(fx_from_int(t.descent), scale));
    int cell_h = baseline + below + 1;

    // Emboldened only when asked AND the file is not already bold. A
    // caller passing a `-bold.ttf` with bold=1 would otherwise get a
    // double-thickened face; there is no way to tell from here, so the
    // rule is the caller's: pass the bold FILE, or pass bold=1, not
    // both. Documented in ugfx.h.
    int smear = bold ? (px / 24 < 1 ? 1 : (px / 24 > 3 ? 3 : px / 24)) : 0;

    int gids[PRIV_SLOTS];
    unsigned char advs[PRIV_SLOTS];
    int cell_w = 1;
    for (int i = 0; i < PRIV_SLOTS; i++) {
        gids[i] = ttf_glyph_index(&t, (uint32_t)(WIN_FONT_FIRST_CHAR + i));
        int adv = ttf_advance_px(&t, gids[i], px);
        if (adv < 0) adv = 0;
        if (adv > 0) adv += smear;
        if (adv > 255) adv = 255;
        advs[i] = (unsigned char)adv;
        if (adv > cell_w) cell_w = adv;
    }

    // The bound in ugfx_font_arena_size() is generous, but it is a
    // bound and not a guarantee about this face -- a display face with
    // absurd metrics could still overrun it. Checked against the arena
    // the caller actually gave, so an overrun is a refusal here rather
    // than a heap corruption in whatever the app allocated next.
    unsigned long glyph_bytes = (unsigned long)PRIV_SLOTS
                              * (unsigned long)cell_w * (unsigned long)cell_h;
    unsigned long need = glyph_bytes + PRIV_SLOTS
                       + (unsigned long)PRIV_SLOTS * PRIV_SLOTS;
    if (cell_h < 2 || need > arena_size) { free(sc); free(file); return 0; }

    unsigned char *blob = (unsigned char *)arena;
    for (unsigned long i = 0; i < need; i++) blob[i] = 0;

    for (int i = 0; i < PRIV_SLOTS; i++) {
        unsigned char *cell = blob + (unsigned long)i * (unsigned long)cell_w
                                                       * (unsigned long)cell_h;
        ttf_render_glyph(&t, gids[i], px, cell, cell_w, cell_h, 0, baseline, sc);
        // Per cell, after rendering it: the cells are contiguous, so
        // emboldening the whole blob at once would smear each glyph
        // into the start of the next (they share rows in memory, not on
        // screen). Ring 0's font_face.c has the same loop for the same
        // reason.
        if (smear) ttf_embolden(cell, cell_w, cell_h, smear);
    }
    for (int i = 0; i < PRIV_SLOTS; i++) blob[glyph_bytes + i] = advs[i];

    signed char *kern = (signed char *)(blob + glyph_bytes + PRIV_SLOTS);
    if (t.kern_pairs) {
        for (int l = 0; l < PRIV_SLOTS; l++)
            for (int r = 0; r < PRIV_SLOTS; r++) {
                int k = ttf_kern_px(&t, gids[l], gids[r], px);
                if (k > 127) k = 127;
                if (k < -127) k = -127;
                kern[l * PRIV_SLOTS + r] = (signed char)k;
            }
    }

    free(sc);
    free(file); // the atlas is self-contained now -- see above

    f->glyphs = blob;
    f->advances = blob + glyph_bytes;
    f->kern = kern;
    f->char_w = cell_w;
    f->char_h = cell_h;
    // A private font is rasterized at full height already (see the
    // ascent/descent comment above), so its bitmap IS its line -- there
    // is no squeeze to hang below.
    f->line_h = cell_h;
    f->count = PRIV_SLOTS;
    return 1;
}

int ugfx_kern(int prev, int c) {
    if (!prev || !g_font->kern) return 0;
    return win_font_kern(g_font->kern, g_glyph_count,
                         glyph_index((unsigned char)prev),
                         glyph_index((unsigned char)c));
}

// How far the pen moves after drawing `c`.
//
// The ring-3 half of gfx_char_advance(), and the reason every
// measurement below stopped multiplying by ugfx_char_w(): with the
// desktop on a proportional face an 'i' is genuinely narrower than a
// 'W', and a client that assumed a fixed cell would draw its labels on
// top of each other. Falls back to the cell width, so a client on the
// baked font behaves exactly as it always did.
int ugfx_char_advance(char c) {
    if (!g_advances) return g_char_w;
    int idx = glyph_index((unsigned char)c);
    int adv = g_advances[idx];
    return adv > 0 ? adv : g_char_w;
}

int ugfx_char_w(void) { return g_char_w; }
// THE LINE PITCH, not the bitmap height -- every caller of this is
// laying something out. Indexing uses the font's char_h directly.
int ugfx_char_h(void) { return g_font->line_h; }

int ugfx_glyph_h(void) { return g_char_h; }

int ugfx_text_width(const char *str) {
    if (!str) return 0;
    int w = 0, prev = 0;
    for (int n = 0; str[n]; n++) {
        w += ugfx_kern(prev, (unsigned char)str[n]) + ugfx_char_advance(str[n]);
        prev = (unsigned char)str[n];
    }
    return w;
}

int ugfx_text_fit_chars(const char *str, int max_w) {
    if (!str || g_char_w <= 0) return 0;
    int n = 0, used = 0, prev = 0;
    while (str[n]) {
        int adv = ugfx_kern(prev, (unsigned char)str[n]) + ugfx_char_advance(str[n]);
        if (used + adv > max_w) break;
        used += adv;
        prev = (unsigned char)str[n];
        n++;
    }
    return n;
}

int ugfx_text_next(const char *str, int i) {
    if (!str || i < 0) return 0;
    return str[i] ? i + 1 : i;
}

int ugfx_text_prev(const char *str, int i) {
    (void)str;
    return i > 0 ? i - 1 : 0;
}

// Blends `fg` over `bg` by `alpha` (0..255), per channel. The glyph
// tables are coverage maps, not masks -- that is what makes this text
// anti-aliased rather than jagged, and it's why a plain "if (a > 128)"
// threshold would visibly degrade it.
static uint32_t blend(uint32_t fg, uint32_t bg, unsigned alpha) {
    unsigned fr = (fg >> 16) & 0xFF, fg_ = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    unsigned br = (bg >> 16) & 0xFF, bg_ = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    unsigned r = (fr * alpha + br * (255 - alpha)) / 255;
    unsigned g = (fg_ * alpha + bg_ * (255 - alpha)) / 255;
    unsigned b = (fb * alpha + bb * (255 - alpha)) / 255;
    return (r << 16) | (g << 8) | b;
}

void ugfx_draw_char(struct ugfx_surface *s, int x, int y, char c,
                     uint32_t color, uint32_t bg) {
    if (!s || !s->pixels || !g_glyphs) return;
    if (x >= s->w || y >= s->h || x + g_char_w <= 0 || y + g_char_h <= 0) return;

    const unsigned char *glyph =
        g_glyphs + win_glyph_offset((uint32_t)glyph_index((unsigned char)c),
                                     g_char_w, g_char_h);

    for (int row = 0; row < g_char_h; row++) {
        int py = y + row;
        if (py < 0 || py >= s->h) continue;
        for (int col = 0; col < g_char_w; col++) {
            int px = x + col;
            if (px < 0 || px >= s->w) continue;
            unsigned a = glyph[row * g_char_w + col];
            if (!a) continue; // fully background -- leave it alone
            // Through put_pixel rather than straight at the buffer, so a
            // glyph obeys the clip rect and marks damage like everything
            // else. The blend is against the CALLER's `bg`, not against
            // what is already there, so nothing is read back.
            ugfx_put_pixel(s, px, py, (a == 255) ? color : blend(color, bg, a));
        }
    }
}

void ugfx_draw_string(struct ugfx_surface *s, int x, int y,
                       const char *str, uint32_t color, uint32_t bg) {
    if (!s || !str) return;
    int gx = x, prev = 0;
    for (int n = 0; str[n]; n++) {
        // The kern comes BEFORE the bounds check, so a run that walks
        // off the right edge stops at the same character
        // ugfx_text_width() would have counted. A drawing loop that
        // spaced text differently from the measuring one is how a
        // clipped label ends up cut in the wrong place.
        gx += ugfx_kern(prev, (unsigned char)str[n]);
        if (gx >= s->w) break; // the rest is off the right edge
        // Nothing special is needed for the overlap a negative kern
        // creates: ugfx_draw_char() skips fully-background pixels
        // outright (`if (!a) continue`), so a kerned glyph composites
        // over its neighbour instead of erasing it. Ring 0's cells ARE
        // opaque and needed draw_glyph_kerned() for exactly this.
        ugfx_draw_char(s, gx, y, str[n], color, bg);
        gx += ugfx_char_advance(str[n]);
        prev = (unsigned char)str[n];
    }
}

int ugfx_draw_string_clipped(struct ugfx_surface *s, int x, int y, int max_w,
                              const char *str, uint32_t color, uint32_t bg) {
    if (!s || !str || !g_glyphs || max_w <= 0) return 0;

    // How many WHOLE glyphs fit. Whole glyphs rather than a pixel clip:
    // half a letter reads as a rendering bug, while a short label just
    // reads as a short label.
    int fits = ugfx_text_fit_chars(str, max_w);
    int len = 0;
    while (str[len]) len++;

    if (fits >= len) {
        ugfx_draw_string(s, x, y, str, color, bg);
        return 1;
    }

    // Draw only what fits, a glyph at a time -- there is no truncated
    // copy of the string because there is nowhere to put one (a client
    // has no allocator), and a fixed scratch buffer would just move the
    // length limit somewhere less obvious.
    int gx = x, prev = 0;
    for (int n = 0; n < fits; n++) {
        char one[2];
        one[0] = str[n];
        one[1] = '\0';
        // The kern is applied HERE rather than inside the one-character
        // ugfx_draw_string() call, which cannot see it: a run of one
        // character has no preceding character, so its own loop always
        // computes 0. Dropping it would space this path differently
        // from the unclipped one above -- the same string, drawn two
        // ways, at two widths.
        gx += ugfx_kern(prev, (unsigned char)str[n]);
        ugfx_draw_string(s, gx, y, one, color, bg);
        gx += ugfx_char_advance(str[n]);
        prev = (unsigned char)str[n];
    }
    return 0;
}

uint32_t ugfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t ugfx_blend(uint32_t under, uint32_t over, uint8_t alpha) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t u = (under >> shift) & 0xFF;
        uint32_t o = (over >> shift) & 0xFF;
        uint32_t v = (u * (255u - alpha) + o * alpha) / 255u;
        out |= (v & 0xFF) << shift;
    }
    return out;
}

uint8_t ugfx_luminance(uint32_t color) {
    // Rec. 601 weights, /256 -- the same ones gfx.c uses, so a client's
    // idea of "is this colour light or dark" matches the desktop's.
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    return (uint8_t)((r * 77 + g * 150 + b * 29) / 256);
}

// --- geometry bindings ------------------------------------------------
//
// The client-side half of the shared geometry module. The only thing
// that differs from the kernel's binding (gfx.c) is where a pixel
// lands: a client's own window buffer rather than the framebuffer.

void ugfx_blend_pixel(struct ugfx_surface *s, int x, int y, uint32_t color, uint8_t alpha) {
    if (!s || !s->pixels) return;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    // Tests the clip BEFORE reading the destination, not just before
    // writing it: this is the one primitive here that reads back, and a
    // read outside the clip is still a read of a pixel this call has no
    // business touching.
    if (s->clip_active &&
        (x < s->clip_x0 || x >= s->clip_x1 || y < s->clip_y0 || y >= s->clip_y1)) return;
    uint32_t *p = &s->pixels[(uint32_t)y * (uint32_t)s->w + (uint32_t)x];
    *p = (alpha >= 255) ? color : blend(color, *p, alpha);
    dirty_mark(s, x, y);
}

static void ugfx_geom_plot(void *ctx, int x, int y, uint32_t color, uint8_t alpha) {
    ugfx_blend_pixel((struct ugfx_surface *)ctx, x, y, color, alpha);
}

// Built per call rather than kept as a global: a client can have more
// than one surface (several windows), and a cached target would quietly
// draw into whichever one was used last.
static struct geom_target target_for(struct ugfx_surface *s) {
    struct geom_target t;
    t.plot = ugfx_geom_plot;
    t.ctx = s;
    return t;
}

void ugfx_draw_line(struct ugfx_surface *s, int x0, int y0, int x1, int y1,
                     uint32_t color, enum geom_aa aa) {
    struct geom_target t = target_for(s);
    geom_line(&t, x0, y0, x1, y1, color, aa);
}

void ugfx_draw_polyline(struct ugfx_surface *s, const int *xs, const int *ys,
                         int count, int closed, uint32_t color, enum geom_aa aa) {
    struct geom_target t = target_for(s);
    geom_polyline(&t, xs, ys, count, closed, color, aa);
}

void ugfx_draw_circle(struct ugfx_surface *s, int cx, int cy, int r,
                       uint32_t color, enum geom_aa aa) {
    struct geom_target t = target_for(s);
    geom_circle(&t, cx, cy, r, color, aa);
}

void ugfx_draw_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry,
                        uint32_t color, enum geom_aa aa) {
    struct geom_target t = target_for(s);
    geom_ellipse(&t, cx, cy, rx, ry, color, aa);
}

void ugfx_fill_circle(struct ugfx_surface *s, int cx, int cy, int r, uint32_t color) {
    struct geom_target t = target_for(s);
    geom_fill_circle(&t, cx, cy, r, color);
}

void ugfx_fill_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry,
                        uint32_t color) {
    struct geom_target t = target_for(s);
    geom_fill_ellipse(&t, cx, cy, rx, ry, color);
}

// --- the screen -------------------------------------------------------
//
// See ugfx.h for the three properties this has to respect. The one that
// governs the code below: the mapped framebuffer is WRITE-COMBINING, so
// every access here is a store. Nothing in this section reads it.

int ugfx_screen_init(struct ugfx_screen *sc) {
    if (!sc) return 0;
    for (unsigned i = 0; i < sizeof *sc; i++) ((uint8_t *)sc)[i] = 0;

    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_FB_MAP;
    // 0 is success for this request, unlike WIN_REQ_FONT's 1 -- see
    // win_proto.h. Refusal is the ordinary answer for a process that is
    // not the registered compositor, so it returns rather than faults.
    if (sys_win_request(&req) != 0) return 0;

    int w = req.a, h = req.b;
    sc->pitch = (uint32_t)req.c;
    sc->bpp = req.d;

    if (w <= 0 || h <= 0) return 0;
    // 24 and 32 are what the kernel's own present path handles, so they
    // are what this handles. Anything else is refused here rather than
    // producing a sheared picture nobody would read as a format problem.
    if (sc->bpp != 32 && sc->bpp != 24) return 0;
    // A pitch too small to hold a row means the grant and this code
    // disagree about the format, and drawing anyway writes off the end
    // of every row.
    if (sc->pitch < (uint32_t)w * (uint32_t)(sc->bpp / 8)) return 0;

    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    void *back = sys_sbrk((int64_t)(pixels * sizeof(uint32_t)));
    if (back == (void *)-1) return 0; // heap ceiling -- see kernel/uaddr.h

    sc->back.pixels = (uint32_t *)back;
    sc->back.w = w;
    sc->back.h = h;
    return 1;
}

void ugfx_screen_present(struct ugfx_screen *sc) {
    if (!sc || !sc->back.pixels) return;

    int x, y, w, h;
    // Nothing drawn since the last present. Not an error, and skipping is
    // the whole reason the damage box exists -- returning here also means
    // an idle compositor makes no syscall at all.
    if (!ugfx_damage(&sc->back, &x, &y, &w, &h)) return;

    int bytes = sc->bpp / 8;
    volatile uint8_t *fb = (volatile uint8_t *)(uintptr_t)WIN_FB_VADDR;

    for (int j = 0; j < h; j++) {
        const uint32_t *src = sc->back.pixels
                            + (uint32_t)(y + j) * (uint32_t)sc->back.w + (uint32_t)x;
        volatile uint8_t *dst = fb + (uint32_t)(y + j) * sc->pitch + (uint32_t)x * bytes;
        if (bytes == 4) {
            // One 32-bit store per pixel, not three 8-bit ones. Not
            // micro-optimisation: on real hardware this is write-combined
            // MMIO where each store is a bus transaction, so three per
            // pixel costs three times the whole blit. Free under TCG,
            // whose framebuffer is cached host RAM -- which is exactly why
            // this has to be measured under KVM if it is claimed at all.
            volatile uint32_t *d32 = (volatile uint32_t *)dst;
            for (int i = 0; i < w; i++) *d32++ = *src++;
        } else {
            for (int i = 0; i < w; i++) {
                uint32_t c = *src++;
                dst[0] = (uint8_t)(c & 0xFF);
                dst[1] = (uint8_t)((c >> 8) & 0xFF);
                dst[2] = (uint8_t)((c >> 16) & 0xFF);
                dst += bytes;
            }
        }
    }

    // Publish BEFORE clearing the damage: on an adapter declaring
    // DISPLAY_CAP_NEEDS_FLUSH the pixels above are still invisible, and
    // dropping the record of what changed first would leave nothing to
    // tell it about.
    struct win_request_msg pr;
    for (unsigned i = 0; i < sizeof pr; i++) ((uint8_t *)&pr)[i] = 0;
    pr.type = WIN_REQ_FB_PRESENT;
    pr.a = x; pr.b = y; pr.c = w; pr.d = h;
    sys_win_request(&pr);

    ugfx_damage_reset(&sc->back);
}

// --- damage verification ----------------------------------------------

int ugfx_verify_snapshot(struct ugfx_screen *sc) {
    if (!sc || !sc->back.pixels) return 0;
    uint32_t n = (uint32_t)sc->back.w * (uint32_t)sc->back.h;
    if (!sc->snapshot) {
        void *p = sys_sbrk((int64_t)((uint64_t)n * sizeof(uint32_t)));
        // A second full screen may simply not fit. Reporting that as
        // "cannot verify" rather than limping on is the point: a verifier
        // that quietly measures nothing calls every frame clean.
        if (p == (void *)-1) return 0;
        sc->snapshot = (uint32_t *)p;
    }
    for (uint32_t i = 0; i < n; i++) sc->snapshot[i] = sc->back.pixels[i];
    sc->snapshot_valid = 1;
    return 1;
}

int ugfx_verify_diff(struct ugfx_screen *sc, struct ugfx_diff *out) {
    return ugfx_verify_diff_masked(sc, out, 0, 0);
}

static int in_skip(const struct ugfx_skip_rect *skip, int nskip, int x, int y) {
    for (int i = 0; i < nskip; i++) {
        if (x >= skip[i].x && x < skip[i].x + skip[i].w &&
            y >= skip[i].y && y < skip[i].y + skip[i].h) return 1;
    }
    return 0;
}

int ugfx_verify_diff_masked(struct ugfx_screen *sc, struct ugfx_diff *out,
                             const struct ugfx_skip_rect *skip, int nskip) {
    struct ugfx_diff d = { 0, -1, -1, 0, 0, 0, 0 };
    if (!sc || !sc->back.pixels || !sc->snapshot || !sc->snapshot_valid) {
        if (out) *out = d;
        return 0;
    }
    for (int y = 0; y < sc->back.h; y++) {
        for (int x = 0; x < sc->back.w; x++) {
            uint32_t i = (uint32_t)y * (uint32_t)sc->back.w + (uint32_t)x;
            if (sc->back.pixels[i] == sc->snapshot[i]) continue;
            if (nskip && in_skip(skip, nskip, x, y)) continue;
            if (d.count == 0) {
                d.first_x = x; d.first_y = y;
                d.x0 = x; d.y0 = y; d.x1 = x + 1; d.y1 = y + 1;
            } else {
                if (x < d.x0) d.x0 = x;
                if (x + 1 > d.x1) d.x1 = x + 1;
                if (y + 1 > d.y1) d.y1 = y + 1; // scan order fixed y0 already
            }
            d.count++;
        }
    }
    if (out) *out = d;
    return d.count;
}

void ugfx_verify_release(struct ugfx_screen *sc) {
    // The buffer itself is deliberately NOT returned -- see ugfx.h.
    // Only the claim that it holds something comparable is dropped.
    if (sc) sc->snapshot_valid = 0;
}
