// ugfx -- see ugfx.h for what this is and why the font arrives the way
// it does.
#include "ui/ugfx.h"
#include "lib/ufile.h"
#include "syscall_abi.h"
#include "rt/sys.h" // sys_sbrk, sys_win_request -- the screen half, below
#include "ttf.h"    // the SAME rasterizer the kernel uses -- see ugfx_font_load
#include "font_shm.h" // the session atlas /bin/fontd publishes
#include "ui/ulog.h"  // which source a client got its glyphs from
#include <stdlib.h>
#include <stdio.h>   // snprintf -- the shm object's name

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
// FOUR OF THEM AND NOT ONE, because every (family, weight) is live at
// the same time. That is the difference between these and a SIZE: the
// machine is only ever at one size, so a size change re-maps in place,
// while a widget picks a weight per run of text -- and a terminal in
// the same process picks the monospace family -- and they need to be
// there at once. UGFX_FONT_SLOT is the index; abi/font_shm.h's
// FONT_SHM_SLOT is the same arithmetic on the publishing side.
static struct ugfx_font g_session[UGFX_FONT_SLOTS];

// What text draws with right now -- always one of the above, or a font
// a client rasterized for itself (ugfx_font_load). Never NULL after a
// successful init.
static const struct ugfx_font *g_font = &g_session[UGFX_FONT_SLOT_UI_REGULAR];

// Which slots have been asked for since the last ugfx_font_init()?
// See ugfx_font_session(): mapping them eagerly cost every client a
// startup round trip per slot, for slots most never use. Slot 0 is
// mapped by init and is always set.
static int g_slot_asked[UGFX_FONT_SLOTS];

// Kept as the old module-level names so that everything below reads as
// it did; they now just track g_font.
#define g_glyphs      (g_font->glyphs)
#define g_advances    (g_font->advances)
#define g_char_w      (g_font->char_w)
// The BITMAP height -- glyph indexing and the drawing loop. Layout goes
// through ugfx_char_h(), which returns the line pitch instead.
#define g_char_h      (g_font->char_h)
#define g_glyph_count (g_font->count)

// A window's drawing surface: whichever of its TWO buffers is currently
// the BACK one (abi/win_proto.h's WIN_BUFFER_HALF). `front` is what the
// server last reported -- 0 for a window that has never presented, and
// for a single-buffered one, which is why a caller needs no special
// case for either.
// A surface over memory the CALLER owns. Every window surface is one
// now: a client allocates its own pixels and knows where it mapped
// them, so there is nothing to derive from a window id.
struct ugfx_surface ugfx_surface_for_pixels(void *pixels, int w, int h) {
    struct ugfx_surface s;
    for (unsigned i = 0; i < sizeof s; i++) ((uint8_t *)&s)[i] = 0;
    s.pixels = (uint32_t *)pixels;
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

void ugfx_clip_save(const struct ugfx_surface *s, struct ugfx_clip *out) {
    out->x0 = s->clip_x0; out->y0 = s->clip_y0;
    out->x1 = s->clip_x1; out->y1 = s->clip_y1;
    out->active = s->clip_active;
}

void ugfx_clip_restore(struct ugfx_surface *s, const struct ugfx_clip *c) {
    s->clip_x0 = c->x0; s->clip_y0 = c->y0;
    s->clip_x1 = c->x1; s->clip_y1 = c->y1;
    s->clip_active = c->active;
}

void ugfx_clip_intersect(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (!s->clip_active) { ugfx_set_clip_rect(s, x, y, w, h); return; }
    int x0 = x > s->clip_x0 ? x : s->clip_x0;
    int y0 = y > s->clip_y0 ? y : s->clip_y0;
    int x1 = x + w < s->clip_x1 ? x + w : s->clip_x1;
    int y1 = y + h < s->clip_y1 ? y + h : s->clip_y1;
    // An empty intersection is an EMPTY clip -- the same rule as
    // ugfx_set_clip_rect(), reached through it.
    ugfx_set_clip_rect(s, x0, y0, x1 - x0, y1 - y0);
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

// THE SAME GROWTH, FOR A RASTERISER IN ANOTHER FILE. ugfx_tex.c writes
// pixels directly for speed, so it cannot go through a drawing call that
// would have marked the rect for it -- and a surface whose dirty rect
// does not cover what was drawn is presented with the new pixels left
// off, which reads as the drawing having never happened.
void ugfx_mark_dirty_rect(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (!s) return;
    dirty_mark_rect(s, x, y, w, h);
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

void ugfx_blit_scaled_alpha(struct ugfx_surface *s, int x, int y, int w, int h,
                             const uint32_t *src, int sw, int sh, int src_pitch_px,
                             uint8_t alpha) {
    if (!s || !s->pixels || !src || w <= 0 || h <= 0 || sw <= 0 || sh <= 0 || alpha == 0) return;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clip_rect(s, &dx, &dy, &dw, &dh)) return;
    unsigned a = alpha;
    // THE SOURCE COLUMN, WITHOUT A DIVISION PER PIXEL. This was
    // `(dx - x + i) * sw / w` inside the inner loop -- a 64-bit divide
    // for every pixel written, and the divisor is a runtime value so
    // nothing strength-reduces it. A full-size window ghost is ~910k
    // pixels, which measured as nearly all of a 19.6 ms animation frame
    // on the bare-metal 1080p panel (docs/bugs.md).
    //
    // Bresenham produces the IDENTICAL floor((col0 + i) * sw / w) from
    // an add and a compare. The two divisions left seed the clipped
    // start column, once per blit rather than once per pixel.
    int col0 = dx - x;
    int sx_start  = (int)((long long)col0 * sw / w);
    int err_start = (int)((long long)col0 * sw % w);
    for (int j = 0; j < dh; j++) {
        // The destination row's source row, in the UNCLIPPED mapping, so
        // a clipped blit shows the same part of the source as a whole one.
        int sy = (int)((long long)(dy - y + j) * sh / h);
        if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = src + (uint32_t)sy * (uint32_t)src_pitch_px;
        uint32_t *drow = s->pixels + (uint32_t)(dy + j) * (uint32_t)s->w + (uint32_t)dx;
        int sx_i = sx_start, err = err_start;
        for (int i = 0; i < dw; i++) {
            int sx = sx_i;
            if (sx >= sw) sx = sw - 1;
            // Advance to the next destination column's source column.
            err += sw;
            while (err >= w) { err -= w; sx_i++; }
            uint32_t sp = srow[sx] & 0x00FFFFFF;
            if (a == 255) { drow[i] = sp; continue; }
            uint32_t dp = drow[i], out = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                unsigned sc = (sp >> shift) & 0xFF, dc = (dp >> shift) & 0xFF;
                unsigned v = (sc * a + dc * (255 - a) + 127) / 255;
                out |= (v & 0xFF) << shift;
            }
            drow[i] = out;
        }
    }
    dirty_mark_rect(s, dx, dy, dw, dh);
}

void ugfx_blit_tinted(struct ugfx_surface *s, int x, int y, int w, int h,
                       const uint32_t *src, int src_pitch_px, uint32_t color) {
    if (!s || !s->pixels || !src || w <= 0 || h <= 0) return;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clip_rect(s, &dx, &dy, &dw, &dh)) return;
    int sx = dx - x, sy = dy - y;

    for (int j = 0; j < dh; j++) {
        const uint32_t *srow = src + (uint32_t)(sy + j) * (uint32_t)src_pitch_px + (uint32_t)sx;
        uint32_t *drow = s->pixels + (uint32_t)(dy + j) * (uint32_t)s->w + (uint32_t)dx;
        for (int i = 0; i < dw; i++) {
            unsigned a = srow[i] >> 24;     // the MASK; the rest is discarded
            if (a == 0) continue;
            if (a == 255) { drow[i] = color & 0x00FFFFFF; continue; }
            uint32_t dp = drow[i];
            uint32_t out = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                unsigned sc = (color >> shift) & 0xFF;
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

// --- the session font, from /bin/fontd -------------------------------
//
// THE ATLAS IS RASTERIZED IN RING 3 NOW, by a service, and a client maps
// it read-only out of shared memory (abi/font_shm.h). The kernel is not
// in this path: it draws its own console from the baked tables and never
// parses a font.
//
// The kernel request below is the FALLBACK, and it is a real one rather
// than a leftover -- a `builtin` face publishes nothing, and a machine
// whose fontd has not started yet still has to draw. What it hands out
// is the baked tables, which is what the console is using anyway.
static uint32_t g_shm_generation[UGFX_FONT_SLOTS];

// The beacon, mapped once. Reading it is a memory access rather than a
// syscall, which is what lets a client check on every frame -- see
// abi/font_shm.h on why the atlas objects cannot answer this themselves.
static const struct font_beacon *g_beacon;
static uint32_t g_seen_beacon;
static uint32_t g_tried_gen = 0xFFFFFFFFu; // no upgrade attempted yet

// What this process currently has mapped for each weight, so a
// republish can give it back.
static void *g_map_base[UGFX_FONT_SLOTS];
static uint64_t g_map_bytes[UGFX_FONT_SLOTS];

// Whether this weight is currently drawn from fontd's atlas, as opposed
// to the kernel's baked fallback.
static int g_from_fontd[UGFX_FONT_SLOTS];

static void fontd_unmap(int slot) {
    if (!g_map_base[slot]) return;
    sys_munmap(g_map_base[slot], g_map_bytes[slot]);
    g_map_base[slot] = 0;
    g_map_bytes[slot] = 0;
}

// **SYS_MMAP FAILS WITH (void *)-1, NOT NULL** -- mmap's own contract,
// stated in rt/sys.h. Testing for NULL passes a failed mapping straight
// through, and the first read of it faults; that is what took the
// compositor down after a few font switches, which is also what made
// mmap start failing (see fontd_unmap below).
#define MAP_BAD(p) ((p) == 0 || (p) == (void *)(intptr_t)-1)

static void beacon_map(void) {
    if (g_beacon) return;
    int fd = sys_shm_open(FONT_BEACON_NAME, 0, 0);
    if (fd < 0) return;
    void *p = sys_mmap(0, sizeof(struct font_beacon), SYS_PROT_READ,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (MAP_BAD(p)) return;
    const struct font_beacon *b = (const struct font_beacon *)p;
    if (b->magic != FONT_BEACON_MAGIC) { sys_munmap(p, sizeof *b); return; }
    g_beacon = b;
    g_seen_beacon = b->generation;
}

static int map_fontd_font(int slot, struct ugfx_font *out) {
    char name[SHM_NAME_MAX];
    snprintf(name, sizeof name, FONT_SHM_NAME_FMT, slot);

    int fd = sys_shm_open(name, 0, 0);   // open existing, never create
    if (fd < 0) return 0;

    // MAP ONE PAGE FIRST to learn the size, then the whole object. An
    // shm object is MAPPED, not read -- sys_read() on this fd answers
    // nothing -- and guessing a length instead either truncates a large
    // atlas or asks for pages that are not there, which SYS_MMAP refuses.
    struct font_shm hdr;
    void *probe = sys_mmap(0, 4096, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    if (MAP_BAD(probe)) { sys_close(fd); return 0; }
    hdr = *(const struct font_shm *)probe;
    sys_munmap(probe, 4096);

    if (hdr.magic != FONT_SHM_MAGIC || hdr.version != FONT_SHM_VERSION ||
        !hdr.generation || !hdr.count || !hdr.cell_w || !hdr.cell_h) {
        // Not ready, or a fontd newer than this client. Fall back rather
        // than read fields that may have moved.
        sys_close(fd);
        return 0;
    }

    void *base = sys_mmap(0, hdr.bytes, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);   // the mapping holds its own reference
    if (MAP_BAD(base)) return 0;

    // **THE PREVIOUS ATLAS IS UNMAPPED, and forgetting to was not a
    // leak that merely wasted memory.** A republish makes a NEW object,
    // so every font change added a whole atlas to this process's address
    // space -- and a few switches in System Settings were enough for the
    // next mmap to fail, which the missing MAP_FAILED check above then
    // turned into a page fault in the compositor.
    fontd_unmap(slot);
    g_map_base[slot] = base;
    g_map_bytes[slot] = hdr.bytes;
    g_from_fontd[slot] = 1;

    const struct font_shm *h = (const struct font_shm *)base;
    out->char_w   = (int)h->cell_w;
    out->char_h   = (int)h->cell_h;
    out->line_h   = (int)h->line_h;
    out->count    = (int)h->count;
    out->glyphs   = (const unsigned char *)base + h->glyph_off;
    out->advances = (const unsigned char *)base + h->adv_off;
    out->kern     = (const signed char *)((const unsigned char *)base + h->kern_off);
    g_shm_generation[slot] = h->generation;
    beacon_map();

    // SAID ONCE PER SLOT, because fontd and the kernel produce the SAME
    // bytes for the baked face -- so nothing on screen can tell you
    // which one a client is drawing from, and "it looks right" is not
    // evidence that this path ran at all.
    ulogf("ugfx: session font slot %d from fontd -- %s, %ux%u cell, line %u, gen %u\n",
          slot, h->monospace ? "monospace" : "proportional",
          h->cell_w, h->cell_h, h->line_h, h->generation);
    return 1;
}

// Asks the server for one slot and fills `out`. Returns 1 on success.
//
// **THE KERNEL FALLBACK HAS NO FAMILIES, and does not need any.** Its
// baked tables are monospace, so a mono slot falling back to them is
// exactly right, and a UI slot falling back to them is what every
// client did before this existed. Only the WEIGHT crosses that seam.
static int map_session_font(int slot, struct ugfx_font *out) {
    if (map_fontd_font(slot, out)) return 1;
    int weight = slot % UGFX_FONT_WEIGHTS;
    g_from_fontd[slot] = 0;
    ulogf("ugfx: session font slot %d from the KERNEL (no fontd atlas)\n", slot);

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

// HAS THE SESSION FONT BEEN REPUBLISHED? A memory read, not a syscall,
// so an app may ask on every frame. Returns 1 when it re-mapped, which
// is the caller's cue to re-measure everything.
//
// It exists because WIN_EV_FONT arrives when the SETTING changes and
// fontd republishes on its own poll a moment later -- so an app that
// only re-mapped on the event would map the OLD atlas again and keep it.
const char *ugfx_font_session_face(void) {
    const struct font_shm *h =
        (const struct font_shm *)g_map_base[UGFX_FONT_SLOT_UI_REGULAR];
    if (!g_from_fontd[UGFX_FONT_SLOT_UI_REGULAR] || !h || !h->face[0]) return 0;
    return h->face;
}

uint32_t ugfx_font_generation(void) { return g_seen_beacon; }

int ugfx_font_recheck(void) {
    beacon_map();
    if (!g_beacon) return 0;

    // **A CLIENT THAT STARTED BEFORE fontd IS STUCK OTHERWISE.** It fell
    // back to the kernel's baked tables, and the first beacon read
    // records whatever generation is already there -- so nothing ever
    // looks like it moved and the client draws the boot font forever,
    // in a different typeface from every window beside it. Asking
    // whether we are ON the atlas is the condition that covers both
    // that and an ordinary republish.
    // ONCE PER REPUBLISH, NOT ONCE PER TICK. A machine whose face is
    // `builtin` publishes nothing on purpose, so this condition is
    // permanently true there -- retrying every frame re-maps the font
    // and resets the current weight on each one, which is a storm, not
    // a fallback.
    if (!g_from_fontd[UGFX_FONT_SLOT_UI_REGULAR]) {
        if (g_beacon->generation == g_tried_gen) return 0;
        g_tried_gen = g_beacon->generation;
        g_seen_beacon = g_beacon->generation;
        return ugfx_font_init() && g_from_fontd[UGFX_FONT_SLOT_UI_REGULAR];
    }

    if (g_beacon->generation == g_seen_beacon) return 0;
    g_seen_beacon = g_beacon->generation;
    ugfx_font_init();
    return 1;
}

int ugfx_font_init(void) {
    if (!map_session_font(UGFX_FONT_SLOT_UI_REGULAR,
                          &g_session[UGFX_FONT_SLOT_UI_REGULAR]))
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
    // EVERY OTHER SLOT'S OWN MAPPING GOES BACK HERE. Each is about to be
    // replaced by a copy of UI-regular's, so keeping one would strand a
    // whole atlas in this address space on every font change -- the leak
    // that made mmap fail and took the compositor down.
    //
    // The copy is what makes an unasked-for slot DRAWABLE rather than
    // empty: a widget that asks for bold, or a terminal that asks for
    // mono, before either is mapped gets UI-regular and draws text,
    // which is the degradation GDI makes for a family with no bold.
    for (int i = 0; i < UGFX_FONT_SLOTS; i++) {
        if (i == UGFX_FONT_SLOT_UI_REGULAR) continue;
        fontd_unmap(i);
        g_session[i] = g_session[UGFX_FONT_SLOT_UI_REGULAR];
        g_slot_asked[i] = 0;
    }
    g_slot_asked[UGFX_FONT_SLOT_UI_REGULAR] = 1;

    // A re-init (WIN_EV_FONT) must not leave the current font pointing
    // at a private atlas whose backing the app may have freed, so this
    // resets to the UI family's regular weight -- which is also what an
    // app expects after the desktop's font changed under it.
    g_font = &g_session[UGFX_FONT_SLOT_UI_REGULAR];
    return 1;
}

// One slot, mapped on FIRST USE. See ugfx_font_init(): every slot but
// UI-regular starts as a copy of it, so this always returns something
// drawable whether or not the map succeeds.
static const struct ugfx_font *session_slot(int slot) {
    if (slot < 0 || slot >= UGFX_FONT_SLOTS) slot = UGFX_FONT_SLOT_UI_REGULAR;
    if (!g_slot_asked[slot]) {
        // Set FIRST: a failed map must not retry on every draw.
        g_slot_asked[slot] = 1;
        map_session_font(slot, &g_session[slot]);
    }
    return &g_session[slot];
}

const struct ugfx_font *ugfx_font_session(int weight) {
    if (weight < 0 || weight >= UGFX_FONT_WEIGHTS) weight = UGFX_FONT_REGULAR;
    return session_slot(UGFX_FONT_SLOT(UGFX_FONT_FAMILY_UI, weight));
}

const struct ugfx_font *ugfx_font_mono(int weight) {
    if (weight < 0 || weight >= UGFX_FONT_WEIGHTS) weight = UGFX_FONT_REGULAR;
    return session_slot(UGFX_FONT_SLOT(UGFX_FONT_FAMILY_MONO, weight));
}

const struct ugfx_font *ugfx_font_current(void) { return g_font; }

const struct ugfx_font *ugfx_set_font(const struct ugfx_font *f) {
    const struct ugfx_font *prev = g_font;
    // NULL means "back to the session's regular weight" rather than
    // "no font": every measurement below would return 0 with a NULL
    // font, and a widget that forgot to restore would collapse the
    // layout of everything drawn after it instead of looking wrong.
    g_font = (f && f->glyphs) ? f : &g_session[UGFX_FONT_SLOT_UI_REGULAR];
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
    // lib/ufile.h, which owns the short-read loop this used to spell
    // out. Nothing here distinguishes the failures -- a face that will
    // not load falls back to the built-in one either way.
    unsigned char *file = 0;
    size_t size = 0;
    if (ufile_slurp(path, 4ul * 1024 * 1024, &file, &size) != UFILE_OK) return 0;

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

// The session size doubled, bold, for a clock or a figure that is the
// point of its panel -- loaded once per cell height and kept. Falls
// back to the session's bold weight when the file will not load.
const struct ugfx_font *ugfx_font_display(void) {
    static struct ugfx_font f;
    static void *arena;
    static int loaded_px, ok;
    // From the SESSION font, never the current one: asked while this
    // font is selected, ugfx_char_h() would double the double.
    int px = ugfx_font_session(UGFX_FONT_REGULAR)->line_h * 2;
    if (px < 16) px = 16;
    if (px != loaded_px) {
        loaded_px = px;
        free(arena);
        unsigned long need = ugfx_font_arena_size(px);
        arena = malloc(need);
        ok = arena && ugfx_font_load("/usr/share/fonts/liberation-sans-bold.ttf",
                                     px, 0, &f, arena, need);
    }
    return ok ? &f : ugfx_font_session(UGFX_FONT_BOLD);
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

// The six string-measurement functions moved to ugfx_text.c: they are
// pure arithmetic over ugfx_char_advance()/ugfx_kern() with no font
// state and no surface of their own, which is what lets a host harness
// compile them against a synthetic proportional face
// (tools/ugfx_text_hostcheck.py). The two accessors they stand on stay
// here, with the atlas.

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
            // what is already there, so nothing is read back -- UNLESS
            // the caller asked for UGFX_TRANSPARENT, which is the one
            // path that reads the surface back (see ugfx.h for the two
            // costs of that).
            if (a == 255) {
                ugfx_put_pixel(s, px, py, color);
            } else {
                uint32_t under = (bg == UGFX_TRANSPARENT)
                                     ? ugfx_get_pixel(s, px, py) : bg;
                ugfx_put_pixel(s, px, py, blend(color, under, a));
            }
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

// See ugfx.h. The shadow is picked from the ink's own luminance -- the
// same "derive the contrast, do not hand-pick it" rule uui.h's
// ui_state_bg() states, and for the same reason: a hand-picked dark
// shadow is invisible under dark text, and this project has already
// shipped the equivalent mistake as a hover nobody could see.
static uint32_t shadow_for(uint32_t color) {
    return ugfx_luminance(color) < 128 ? ugfx_rgb(255, 255, 255)
                                       : ugfx_rgb(0, 0, 0);
}

void ugfx_draw_string_shadowed(struct ugfx_surface *s, int x, int y,
                                const char *str, uint32_t color) {
    ugfx_draw_string(s, x + 1, y + 1, str, shadow_for(color), UGFX_TRANSPARENT);
    ugfx_draw_string(s, x, y, str, color, UGFX_TRANSPARENT);
}

int ugfx_draw_string_clipped_shadowed(struct ugfx_surface *s, int x, int y,
                                       int max_w, const char *str, uint32_t color) {
    // The SHADOW is clipped to the same max_w as the ink, not to one a
    // pixel wider: a shadow surviving one character past the text it
    // belongs to is a ghost letter, which is worse than a clipped
    // shadow nobody can see is clipped.
    ugfx_draw_string_clipped(s, x + 1, y + 1, max_w, str, shadow_for(color),
                             UGFX_TRANSPARENT);
    return ugfx_draw_string_clipped(s, x, y, max_w, str, color, UGFX_TRANSPARENT);
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

void ugfx_blend_hspan(struct ugfx_surface *s, int x, int y, int w,
                       uint32_t color, const uint8_t *cov, uint8_t alpha) {
    if (!s || !s->pixels || w <= 0) return;
    if (y < 0 || y >= s->h) return;
    int x0 = x, x1 = x + w;
    if (x0 < 0) x0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (s->clip_active) {
        if (y < s->clip_y0 || y >= s->clip_y1) return;
        if (x0 < s->clip_x0) x0 = s->clip_x0;
        if (x1 > s->clip_x1) x1 = s->clip_x1;
    }
    if (x1 <= x0) return;
    uint32_t *p = &s->pixels[(uint32_t)y * (uint32_t)s->w + (uint32_t)x0];
    if (cov) {
        const uint8_t *c = cov + (x0 - x);   // clipping skipped that much coverage too
        for (int i = 0; i < x1 - x0; i++) {
            unsigned al = c[i];
            if (al) p[i] = (al >= 255) ? color : blend(color, p[i], al);
        }
    } else if (alpha) {
        if (alpha >= 255) for (int i = 0; i < x1 - x0; i++) p[i] = color;
        else              for (int i = 0; i < x1 - x0; i++) p[i] = blend(color, p[i], alpha);
    } else {
        return;                              // nothing drawn, so nothing dirtied
    }
    dirty_mark(s, x0, y);
    dirty_mark(s, x1 - 1, y);
}

// Smoothstep in Q16.16: 3t^2 - 2t^3, the curve that gives the genie
// its neck. Flat at both ends, so the window's own edge and the
// button's edge each meet the tube without a visible corner.
static uint32_t smoothstep_fx(uint32_t t) {
    if (t >= 65536u) return 65536u;
    uint64_t tt = ((uint64_t)t * t) >> 16;              // t^2
    uint64_t ttt = (tt * t) >> 16;                      // t^3
    uint64_t v = 3u * tt - 2u * ttt;
    return v > 65536u ? 65536u : (uint32_t)v;
}

static int lerp_i(int a, int b, uint32_t t) {
    return a + (int)(((long long)(b - a) * (long long)t) >> 16);
}

void ugfx_blit_genie(struct ugfx_surface *s, int y0, int h,
                      int top_cx, int top_w, int bot_cx, int bot_w,
                      const uint32_t *src, int sw, int sh, int src_pitch_px,
                      uint8_t alpha) {
    if (!s || !s->pixels || !src || h <= 0 || sw <= 0 || sh <= 0 || alpha == 0) return;

    for (int j = 0; j < h; j++) {
        int dy = y0 + j;
        if (dy < 0 || dy >= s->h) continue;
        if (s->clip_active && (dy < s->clip_y0 || dy >= s->clip_y1)) continue;

        // WHERE THIS ROW SITS IN THE TUBE. The curve is over the row's
        // position in the band, not over time: that is what makes the
        // shape a neck rather than a trapezoid, and what keeps the top
        // looking like a window while the bottom is already a spout.
        uint32_t u = h > 1 ? (uint32_t)(((uint64_t)j << 16) / (uint32_t)(h - 1)) : 65536u;
        uint32_t t = smoothstep_fx(u);
        int rw = lerp_i(top_w, bot_w, t);
        int rcx = lerp_i(top_cx, bot_cx, t);
        if (rw <= 0) continue;

        int rx0 = rcx - rw / 2, rx1 = rx0 + rw;
        // The source row: the WHOLE window is always in the tube,
        // compressed into it, so nothing of it is ever missing.
        int sy = (int)(((long long)j * sh) / h);
        if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = src + (uint32_t)sy * (uint32_t)src_pitch_px;

        int cx0 = rx0 < 0 ? 0 : rx0, cx1 = rx1 > s->w ? s->w : rx1;
        if (s->clip_active) {
            if (cx0 < s->clip_x0) cx0 = s->clip_x0;
            if (cx1 > s->clip_x1) cx1 = s->clip_x1;
        }
        if (cx1 <= cx0) continue;

        // Bresenham across the row, as ugfx_blit_scaled_alpha does and
        // for the same reason: a divide per pixel is what made the
        // ghost cost most of a frame.
        int col0 = cx0 - rx0;
        int sx_i = (int)(((long long)col0 * sw) / rw);
        int err  = (int)(((long long)col0 * sw) % rw);
        uint32_t *drow = s->pixels + (uint32_t)dy * (uint32_t)s->w + (uint32_t)cx0;
        unsigned a = alpha;
        for (int i = 0; i < cx1 - cx0; i++) {
            int sx = sx_i >= sw ? sw - 1 : sx_i;
            err += sw;
            while (err >= rw) { err -= rw; sx_i++; }
            uint32_t sp = srow[sx] & 0x00FFFFFF;
            if (a >= 255) { drow[i] = sp; continue; }
            uint32_t dp = drow[i], out = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                unsigned sc = (sp >> shift) & 0xFF, dc = (dp >> shift) & 0xFF;
                unsigned v = (sc * a + dc * (255 - a) + 127) / 255;
                out |= (v & 0xFF) << shift;
            }
            drow[i] = out;
        }
        dirty_mark(s, cx0, dy);
        dirty_mark(s, cx1 - 1, dy);
    }
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
void ugfx_fill_ring(struct ugfx_surface *s, int cx, int cy,
                     int r_outer, int r_inner, fx_t from, fx_t to,
                     uint32_t color) {
    struct geom_target t = target_for(s);
    geom_fill_ring(&t, cx, cy, r_outer, r_inner, from, to, color);
}


void ugfx_fill_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry,
                        uint32_t color) {
    struct geom_target t = target_for(s);
    geom_fill_ellipse(&t, cx, cy, rx, ry, color);
}

void ugfx_fill_polygon(struct ugfx_surface *s, const int *xs, const int *ys,
                        int count, uint32_t color) {
    struct geom_target t = target_for(s);
    geom_fill_polygon(&t, xs, ys, count, color);
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
    sc->buffers = (int)req.mods;
    if (sc->buffers < 1) sc->buffers = 1;
    if (sc->buffers > UGFX_SCREEN_BUFFERS) sc->buffers = UGFX_SCREEN_BUFFERS;
    sc->back_index = (int)req.window < sc->buffers ? (int)req.window : 0;

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
    sc->back_capacity = (uint32_t)pixels;
    return 1;
}

int ugfx_screen_remode(struct ugfx_screen *sc) {
    if (!sc || !sc->back.pixels) return 0;
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_FB_MAP;
    if (sys_win_request(&req) != 0) return 0;
    int w = req.a, h = req.b;
    if (w <= 0 || h <= 0) return 0;
    if ((req.d != 32 && req.d != 24) || (uint32_t)req.c < (uint32_t)w * (uint32_t)(req.d / 8))
        return 0;
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    if (pixels > sc->back_capacity) {
        // A fresh, larger allocation; the old one stays owned by the
        // heap (sbrk cannot give it back) and is simply unused.
        void *back = sys_sbrk((int64_t)(pixels * sizeof(uint32_t)));
        if (back == (void *)-1) return 0;
        sc->back.pixels = (uint32_t *)back;
        sc->back_capacity = (uint32_t)pixels;
    }
    sc->pitch = (uint32_t)req.c;
    sc->bpp = req.d;
    sc->back.w = w;
    sc->back.h = h;
    sc->buffers = (int)req.mods;
    if (sc->buffers < 1) sc->buffers = 1;
    if (sc->buffers > UGFX_SCREEN_BUFFERS) sc->buffers = UGFX_SCREEN_BUFFERS;
    sc->back_index = (int)req.window < sc->buffers ? (int)req.window : 0;
    sc->seq = 0;
    for (int i = 0; i < UGFX_SCREEN_BUFFERS; i++) sc->painted_seq[i] = 0;
    for (int i = 0; i < UGFX_DAMAGE_RING; i++) sc->dmg_w[i] = sc->dmg_h[i] = 0;
    sc->snapshot_valid = 0;   // a snapshot of the old size compares against nothing
    ugfx_damage_reset(&sc->back);
    return 1;
}

void ugfx_screen_forget(struct ugfx_screen *sc, int back) {
    if (!sc) return;
    if (back >= 0 && back < sc->buffers) sc->back_index = back;
    for (int i = 0; i < UGFX_SCREEN_BUFFERS; i++) sc->painted_seq[i] = 0;
}

// A TEST LEVER, not a setting: presents the WHOLE screen every frame
// when the display flips between several buffers, instead of the
// buffer-age union below. The multi-buffer path runs on NO machine any
// automated test here uses (QEMU reports one scanout and no flip), and
// a screenshot cannot see its output -- wm_screenshot.c copies the BACK
// buffer, having first rendered a frame -- so this is how a human with
// flip-capable hardware tells a catch-up bug from one further down.
static int g_present_full;
void ugfx_screen_present_full(int on) { g_present_full = on ? 1 : 0; }
int  ugfx_screen_present_full_get(void) { return g_present_full; }

void ugfx_screen_present(struct ugfx_screen *sc) {
    if (!sc || !sc->back.pixels) return;

    int x, y, w, h;
    // Nothing drawn since the last present. Not an error, and skipping is
    // the whole reason the damage box exists -- returning here also means
    // an idle compositor makes no syscall at all.
    if (!ugfx_damage(&sc->back, &x, &y, &w, &h)) return;
    int dx = x, dy = y, dw = w, dh = h;   // this frame's own damage

    // BUFFER AGE. The buffer being written was last painted at
    // painted_seq[back]; every frame presented since went to another
    // buffer, so it lacks all of their damage as well as this frame's.
    // Union them from the ring; anything older than the ring, or never
    // painted, gets the whole screen.
    int back = sc->buffers > 1 ? sc->back_index : 0;
    uint32_t this_seq = sc->seq + 1;
    if (sc->buffers > 1) {
        uint32_t last = sc->painted_seq[back];
        uint32_t age = last ? this_seq - last : 0;   // frames since; 0 = never
        if (!last || age > UGFX_DAMAGE_RING || g_present_full) {
            x = 0; y = 0; w = sc->back.w; h = sc->back.h;
        } else {
            int x1 = x + w, y1 = y + h;
            for (uint32_t s = last + 1; s < this_seq; s++) {
                int r = (int)(s % UGFX_DAMAGE_RING);
                if (sc->dmg_w[r] <= 0 || sc->dmg_h[r] <= 0) continue;
                int rx1 = sc->dmg_x[r] + sc->dmg_w[r], ry1 = sc->dmg_y[r] + sc->dmg_h[r];
                if (sc->dmg_x[r] < x) x = sc->dmg_x[r];
                if (sc->dmg_y[r] < y) y = sc->dmg_y[r];
                if (rx1 > x1) x1 = rx1;
                if (ry1 > y1) y1 = ry1;
            }
            w = x1 - x; h = y1 - y;
        }
    }

    int bytes = sc->bpp / 8;
    volatile uint8_t *fb = (volatile uint8_t *)(uintptr_t)
        (WIN_FB_VADDR + (uint64_t)back * WIN_FB_BUFFER_STRIDE);

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
    sc->seq = this_seq;
    if (sys_win_request(&pr) == 0 && sc->buffers > 1) {
        sc->painted_seq[back] = this_seq;
        int next = (int)pr.window < sc->buffers ? (int)pr.window : 0;
        if (next != back) sc->flips++;
        sc->back_index = next;
    }
    sc->presents++;
    // This frame's OWN damage is what the other buffers will lack, not
    // the union just copied.
    int r = (int)(this_seq % UGFX_DAMAGE_RING);
    sc->dmg_x[r] = dx; sc->dmg_y[r] = dy; sc->dmg_w[r] = dw; sc->dmg_h[r] = dh;

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
    struct ugfx_diff d = { 0, -1, -1, 0, 0, 0, 0, 0, 0 };
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
                d.first_was = sc->snapshot[i];
                d.first_now = sc->back.pixels[i];
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
