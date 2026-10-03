// ugfx's glass: a box blur of the surface into a backdrop, and the blit
// that shows a glass client's marked pixels over one (ugfx.h).
//
// THE BLUR IS SEPARABLE AND RUNNING-SUM: three box passes along the rows,
// then three down the columns, each O(1) per pixel whatever the radius --
// the box approximation every software compositor uses rather than a true
// Gaussian kernel. The column passes walk ROWS, keeping a running sum per
// column, because a column-at-a-time walk misses the cache on every pixel.
// A wide blur runs at HALF RESOLUTION and is upsampled bilinearly (what
// KWin's dual-Kawase blur exploits): a blurred picture has no detail for
// the full-size pass to keep. Edges CLAMP to the rect, so nothing outside
// it is ever read.
#include "ui/ugfx.h"
#include <stdint.h>
#include <stdlib.h>

#define MAX_LINE 8192            // the longest row or column blurred
#define PASSES   3
#define HALF_AT  6               // radii from here blur at half resolution

static uint32_t g_a[MAX_LINE], g_b[MAX_LINE];   // main-thread only, as every ugfx call is
static uint32_t g_sum[3 * MAX_LINE];
static uint32_t *g_w, *g_w2;                    // the working picture, two copies
static size_t g_cap;

static int grow(size_t n) {
    if (n <= g_cap) return 1;
    uint32_t *a = realloc(g_w, n * 4), *b = a ? realloc(g_w2, n * 4) : 0;
    if (a) g_w = a;
    if (!b) return 0;
    g_w2 = b;
    g_cap = n;
    return 1;
}

static inline uint32_t avg(uint32_t sr, uint32_t sg, uint32_t sb, uint32_t inv) {
    uint32_t r = (sr * inv + 32768) >> 16, g = (sg * inv + 32768) >> 16, b = (sb * inv + 32768) >> 16;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

// One box pass along a line, `src` -> `dst`, window 2r+1, edges clamped.
static void hbox(const uint32_t *src, uint32_t *dst, int n, int r) {
    uint32_t d = (uint32_t)(2 * r + 1), inv = (65536u + d / 2) / d;
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int k = -r; k <= r; k++) {
        uint32_t p = src[k < 0 ? 0 : (k >= n ? n - 1 : k)];
        sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
    }
    for (int k = 0; k < n; k++) {
        dst[k] = avg(sr, sg, sb, inv);
        int in = k + r + 1, out = k - r;
        uint32_t pi = src[in >= n ? n - 1 : in], po = src[out < 0 ? 0 : out];
        sr += ((pi >> 16) & 0xFF) - ((po >> 16) & 0xFF);
        sg += ((pi >> 8) & 0xFF) - ((po >> 8) & 0xFF);
        sb += (pi & 0xFF) - (po & 0xFF);
    }
}

// One box pass DOWN every column of an n x m picture, a row at a time.
static void vbox(const uint32_t *src, uint32_t *dst, int n, int m, int r) {
    uint32_t d = (uint32_t)(2 * r + 1), inv = (65536u + d / 2) / d;
    uint32_t *sr = g_sum, *sg = g_sum + n, *sb = g_sum + 2 * n;
    for (int c = 0; c < n; c++) sr[c] = sg[c] = sb[c] = 0;
    for (int k = -r; k <= r; k++) {
        const uint32_t *row = src + (size_t)(k < 0 ? 0 : (k >= m ? m - 1 : k)) * n;
        for (int c = 0; c < n; c++) {
            uint32_t p = row[c];
            sr[c] += (p >> 16) & 0xFF; sg[c] += (p >> 8) & 0xFF; sb[c] += p & 0xFF;
        }
    }
    for (int j = 0; j < m; j++) {
        uint32_t *drow = dst + (size_t)j * n;
        int in = j + r + 1, out = j - r;
        const uint32_t *ri = src + (size_t)(in >= m ? m - 1 : in) * n;
        const uint32_t *ro = src + (size_t)(out < 0 ? 0 : out) * n;
        for (int c = 0; c < n; c++) {
            drow[c] = avg(sr[c], sg[c], sb[c], inv);
            uint32_t pi = ri[c], po = ro[c];
            sr[c] += ((pi >> 16) & 0xFF) - ((po >> 16) & 0xFF);
            sg[c] += ((pi >> 8) & 0xFF) - ((po >> 8) & 0xFF);
            sb[c] += (pi & 0xFF) - (po & 0xFF);
        }
    }
}

// Bilinear, between four 0xRRGGBB pixels, weights in 1/256.
static inline uint32_t lerp4(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11,
                             unsigned fx, unsigned fy) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        unsigned a = (p00 >> sh) & 0xFF, b = (p10 >> sh) & 0xFF;
        unsigned c = (p01 >> sh) & 0xFF, e = (p11 >> sh) & 0xFF;
        unsigned top = a * (256 - fx) + b * fx, bot = c * (256 - fx) + e * fx;
        out |= (((top * (256 - fy) + bot * fy) >> 16) & 0xFF) << sh;
    }
    return out;
}

void ugfx_blur_rect(const struct ugfx_surface *s, int x, int y, int w, int h,
                    int radius, uint32_t *out) {
    if (!s || !s->pixels || !out || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
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
    int n = x1 - x0, m = y1 - y0;
    if (n <= 0 || m <= 0 || n > MAX_LINE || m > MAX_LINE) return;
    if (radius < 1) radius = 1;
    int f = radius >= HALF_AT && n > 1 && m > 1 ? 2 : 1;
    int r = radius / f;
    int dn = (n + f - 1) / f, dm = (m + f - 1) / f;
    if (!grow((size_t)dn * (size_t)dm)) return;
    size_t stride = (size_t)s->w;

    // In, a row at a time -- each f x f block averaged -- with the row
    // passes done while the row is hot.
    for (int j = 0; j < dm; j++) {
        const uint32_t *r0 = s->pixels + (size_t)(y0 + j * f) * stride + x0;
        const uint32_t *r1 = (f == 2 && y0 + j * f + 1 < y1) ? r0 + stride : r0;
        for (int k = 0; k < dn; k++) {
            if (f == 1) { g_a[k] = r0[k]; continue; }
            int i0 = 2 * k, i1 = i0 + 1 < n ? i0 + 1 : i0;
            uint32_t q[4] = { r0[i0], r0[i1], r1[i0], r1[i1] }, o = 0;
            for (int sh = 0; sh <= 16; sh += 8)
                o |= ((((q[0] >> sh) & 0xFF) + ((q[1] >> sh) & 0xFF) +
                       ((q[2] >> sh) & 0xFF) + ((q[3] >> sh) & 0xFF) + 2) >> 2) << sh;
            g_a[k] = o;
        }
        uint32_t *src = g_a, *dst = g_b;
        for (int p = 0; p < PASSES; p++) {
            hbox(src, dst, dn, r);
            uint32_t *t = src; src = dst; dst = t;
        }
        uint32_t *wrow = g_w + (size_t)j * dn;
        for (int k = 0; k < dn; k++) wrow[k] = src[k];
    }
    uint32_t *src = g_w, *dst = g_w2;
    for (int p = 0; p < PASSES; p++) {
        vbox(src, dst, dn, dm, r);
        uint32_t *t = src; src = dst; dst = t;
    }

    // Out, at full size.
    for (int j = 0; j < m; j++) {
        uint32_t *orow = out + (size_t)(y0 + j) * stride + x0;
        if (f == 1) {
            const uint32_t *wrow = src + (size_t)j * dn;
            for (int i = 0; i < n; i++) orow[i] = wrow[i];
            continue;
        }
        // Sample centres: full-size pixel i sits at (i + 0.5) / 2 - 0.5.
        int vy = j * 128 - 64, ry = vy < 0 ? 0 : vy >> 8;
        unsigned fy = vy < 0 ? 0 : (unsigned)(vy & 255);
        int ry1 = ry + 1 < dm ? ry + 1 : ry;
        const uint32_t *a = src + (size_t)ry * dn, *b = src + (size_t)ry1 * dn;
        for (int i = 0; i < n; i++) {
            int vx = i * 128 - 64, rx = vx < 0 ? 0 : vx >> 8;
            unsigned fx = vx < 0 ? 0 : (unsigned)(vx & 255);
            int rx1 = rx + 1 < dn ? rx + 1 : rx;
            orow[i] = lerp4(a[rx], a[rx1], b[rx], b[rx1], fx, fy);
        }
    }
}

static inline uint32_t mix(uint32_t under, uint32_t over, unsigned a) {
    unsigned r = (((under >> 16) & 0xFF) * (255 - a) + ((over >> 16) & 0xFF) * a + 127) / 255;
    unsigned g = (((under >> 8) & 0xFF) * (255 - a) + ((over >> 8) & 0xFF) * a + 127) / 255;
    unsigned b = ((under & 0xFF) * (255 - a) + (over & 0xFF) * a + 127) / 255;
    return r << 16 | g << 8 | b;
}

void ugfx_blit_glass(struct ugfx_surface *s, int x, int y, int w, int h,
                     const uint32_t *src, int src_pitch_px,
                     const uint32_t *backdrop, uint8_t alpha) {
    if (!s || !s->pixels || !src || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
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
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t stride = (uint32_t)s->w;
    for (int j = y0; j < y1; j++) {
        const uint32_t *srow = src + (uint32_t)(j - y) * (uint32_t)src_pitch_px + (uint32_t)(x0 - x);
        uint32_t *drow = s->pixels + (uint32_t)j * stride + (uint32_t)x0;
        const uint32_t *brow = backdrop ? backdrop + (uint32_t)j * stride + (uint32_t)x0 : drow;
        for (int i = 0; i < x1 - x0; i++) {
            uint32_t p = srow[i];
            if (!(p >> 24)) { drow[i] = p; continue; }
            drow[i] = alpha >= 255 ? (p & 0xFFFFFF) : mix(brow[i], p, alpha);
        }
    }
    ugfx_mark_dirty_rect(s, x0, y0, x1 - x0, y1 - y0);
}
