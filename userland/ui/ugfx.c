// ugfx -- see ugfx.h for what this is and why the font arrives the way
// it does.
#include "ui/ugfx.h"
#include "syscall_abi.h"
#include "rt/sys.h" // sys_sbrk, sys_win_request -- the screen half, below

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

// Font state, filled by ugfx_font_init(). Zero until then, which makes
// a forgotten init draw nothing rather than dereference a wild pointer
// -- see ugfx.h.
static const unsigned char *g_glyphs = 0;
static int g_char_w = 0;
static int g_char_h = 0;
static int g_glyph_count = 0;

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

int ugfx_font_init(void) {
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_FONT;
    if (syscall2(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)&req, 0) != 1) return 0;

    g_char_w = req.a;
    g_char_h = req.b;
    g_glyph_count = req.c;
    // Glyph 0 sits at the mapping's base PLUS the data's offset within
    // its first page -- the tables are ordinary kernel .rodata and do
    // not start on a page boundary. Ignoring `d` here would shift every
    // glyph by a few bytes and render convincing-looking garbage.
    g_glyphs = (const unsigned char *)(uintptr_t)(WIN_FONT_VADDR + (uint32_t)req.d);
    return 1;
}

int ugfx_char_w(void) { return g_char_w; }
int ugfx_char_h(void) { return g_char_h; }

int ugfx_text_width(const char *str) {
    if (!str) return 0;
    int n = 0;
    while (str[n]) n++;
    return n * g_char_w;
}

int ugfx_text_fit_chars(const char *str, int max_w) {
    if (!str || g_char_w <= 0 || max_w < g_char_w) return 0;
    int n = 0, used = 0;
    while (str[n] && used + g_char_w <= max_w) { used += g_char_w; n++; }
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

// Character -> glyph slot. The shared table is ASCII 32..126 laid out
// contiguously from index 0; anything outside that draws as a space,
// which is the quiet-degradation choice (a client rendering a stray
// byte should look wrong, not read out of bounds).
static int glyph_index(unsigned char c) {
    int idx = (int)c - WIN_FONT_FIRST_CHAR;
    if (idx < 0 || idx >= g_glyph_count) return 0;
    return idx;
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
    for (int n = 0; str[n]; n++) {
        int gx = x + n * g_char_w;
        if (gx >= s->w) break; // the rest is off the right edge
        ugfx_draw_char(s, gx, y, str[n], color, bg);
    }
}

int ugfx_draw_string_clipped(struct ugfx_surface *s, int x, int y, int max_w,
                              const char *str, uint32_t color, uint32_t bg) {
    if (!s || !str || !g_glyphs || max_w <= 0) return 0;

    // How many WHOLE glyphs fit. Whole glyphs rather than a pixel clip:
    // half a letter reads as a rendering bug, while a short label just
    // reads as a short label.
    int fits = max_w / (g_char_w > 0 ? g_char_w : 1);
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
    for (int n = 0; n < fits; n++) {
        char one[2];
        one[0] = str[n];
        one[1] = '\0';
        ugfx_draw_string(s, x + n * g_char_w, y, one, color, bg);
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
    struct ugfx_diff d = { 0, -1, -1, 0, 0, 0, 0 };
    if (!sc || !sc->back.pixels || !sc->snapshot || !sc->snapshot_valid) {
        if (out) *out = d;
        return 0;
    }
    for (int y = 0; y < sc->back.h; y++) {
        for (int x = 0; x < sc->back.w; x++) {
            uint32_t i = (uint32_t)y * (uint32_t)sc->back.w + (uint32_t)x;
            if (sc->back.pixels[i] == sc->snapshot[i]) continue;
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
