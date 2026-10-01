// ugfx's filled shapes -- circles, ellipses, polygons -- ANTI-ALIASED,
// for every caller (Cairo's and Direct2D's default; ugfx.h has why).
//
// COVERAGE, NOT SUPERSAMPLED PIXELS: each pixel row is cut into sixteen
// sub-scanlines (Skia's supersampling depth); on each, a shape's spans
// are measured in 1/256 px and added to that row's coverage exactly, so
// a vertical edge is as smooth as the 1/256 grid and a horizontal one as
// the sixteen sub-rows. A span's whole pixels go through a difference
// array, so a large shape costs O(width) per row, not per sub-row.
//
// THE FOOTPRINT MATCHES THE ALIASED FILL'S: an integer vertex or centre
// means the CENTRE of that pixel (geom.h's pixel-centre rule), and a
// circle of radius r covers r + 1/2 from it -- so a shape drawn here
// covers what geom_fill_* filled, with the edge blended instead of
// stepped.
//
// Pure arithmetic over a surface and ugfx_blend_pixel(), so
// tools/ugfx_fill_hostcheck.py compiles this file on the host and
// checks the coverage against Pillow's rasteriser.
#include "ui/ugfx.h"
#include <stdint.h>

#define SUB      16                // sub-scanlines per pixel row
#define FULL     (256 * SUB)       // one pixel's coverage, fully inside
#define MAX_W    8192              // the widest row a fill covers
#define MAX_XS   (2 * 256)         // crossings on one sub-scanline

static int32_t g_cov[MAX_W];       // main-thread only, as every ugfx call is
static int32_t g_run[MAX_W + 1];   // whole-pixel runs, as a difference array

// One shape, asked for its spans: the sorted x crossings (1/256 px) of
// the horizontal line at `y` (1/256 px). Pairs are inside.
typedef int (*span_fn)(const void *shape, int64_t y, int64_t *xs, int max);

static int64_t fdiv(int64_t a, int64_t b) {        // floor division
    int64_t q = a / b;
    return (a % b && (a < 0) != (b < 0)) ? q - 1 : q;
}

static void fill(struct ugfx_surface *s, int64_t x0f, int64_t y0f, int64_t x1f, int64_t y1f,
                 span_fn spans, const void *shape, uint32_t color) {
    int cx0 = s->clip_active ? s->clip_x0 : 0, cx1 = s->clip_active ? s->clip_x1 : s->w;
    int cy0 = s->clip_active ? s->clip_y0 : 0, cy1 = s->clip_active ? s->clip_y1 : s->h;
    int64_t px0 = fdiv(x0f, 256), px1 = fdiv(x1f, 256) + 1;   // half-open
    int64_t py0 = fdiv(y0f, 256), py1 = fdiv(y1f, 256) + 1;
    if (px0 < cx0) px0 = cx0;
    if (px1 > cx1) px1 = cx1;
    if (py0 < cy0) py0 = cy0;
    if (py1 > cy1) py1 = cy1;
    if (px1 - px0 > MAX_W) px1 = px0 + MAX_W;
    if (px0 >= px1 || py0 >= py1) return;
    int width = (int)(px1 - px0);
    static int64_t xs[MAX_XS];       // 4 KiB: static, as g_cov is -- not a frame's worth

    for (int64_t py = py0; py < py1; py++) {
        for (int i = 0; i < width; i++) g_cov[i] = g_run[i] = 0;
        g_run[width] = 0;
        for (int k = 0; k < SUB; k++) {
            int64_t y = py * 256 + k * (256 / SUB) + 256 / SUB / 2;   // the sub-row's centre
            int n = spans(shape, y, xs, MAX_XS);
            for (int j = 0; j + 1 < n; j += 2) {
                int64_t a = xs[j], b = xs[j + 1];
                if (a < px0 * 256) a = px0 * 256;
                if (b > px1 * 256) b = px1 * 256;
                if (a >= b) continue;
                int64_t pa = fdiv(a, 256), pb = fdiv(b - 1, 256);
                if (pa == pb) { g_cov[pa - px0] += (int32_t)(b - a); continue; }
                g_cov[pa - px0] += (int32_t)((pa + 1) * 256 - a);
                g_run[pa + 1 - px0] += 256;
                g_run[pb - px0] -= 256;
                g_cov[pb - px0] += (int32_t)(b - pb * 256);
            }
        }
        int32_t run = 0;
        for (int i = 0; i < width; i++) {
            run += g_run[i];
            int32_t c = g_cov[i] + run;
            if (c <= 0) continue;
            ugfx_blend_pixel(s, (int)(px0 + i), (int)py, color,
                             c >= FULL ? 255 : (uint8_t)(c * 255 / FULL));
        }
    }
}

// --- an ellipse: one span per sub-row, from the integer square root ----

struct ellipse { int64_t cx, cy, rx, ry; };   // all in 1/256 px

static int64_t isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t r = 0, bit = (int64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static int ellipse_spans(const void *shape, int64_t y, int64_t *xs, int max) {
    const struct ellipse *e = shape;
    int64_t dy = y - e->cy;
    if (max < 2 || dy <= -e->ry || dy >= e->ry) return 0;
    // half-width = rx * sqrt(1 - (dy/ry)^2) = rx * sqrt(ry^2 - dy^2) / ry
    int64_t hw = e->rx * isqrt64(e->ry * e->ry - dy * dy) / e->ry;
    xs[0] = e->cx - hw;
    xs[1] = e->cx + hw;
    return 2;
}

void ugfx_fill_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry, uint32_t color) {
    if (rx < 0 || ry < 0) return;
    struct ellipse e = { (int64_t)cx * 256 + 128, (int64_t)cy * 256 + 128,
                         (int64_t)rx * 256 + 128, (int64_t)ry * 256 + 128 };
    fill(s, e.cx - e.rx, e.cy - e.ry, e.cx + e.rx, e.cy + e.ry, ellipse_spans, &e, color);
}

void ugfx_fill_circle(struct ugfx_surface *s, int cx, int cy, int r, uint32_t color) {
    ugfx_fill_ellipse(s, cx, cy, r, r, color);
}

// --- a polygon: crossings, even-odd ----------------------------------

#define POLY_MAX 256

struct polygon { int64_t x[POLY_MAX], y[POLY_MAX]; int n; };

static int poly_spans(const void *shape, int64_t y, int64_t *xs, int max) {
    const struct polygon *p = shape;
    int n = 0;
    for (int i = 0; i < p->n; i++) {
        int j = (i + 1) % p->n;
        int64_t ay = p->y[i], by = p->y[j];
        // Half-open in y, so a vertex shared by two edges crosses once.
        if ((ay <= y && y < by) || (by <= y && y < ay)) {
            if (n >= max) break;
            xs[n++] = p->x[i] + (y - ay) * (p->x[j] - p->x[i]) / (by - ay);
        }
    }
    for (int i = 1; i < n; i++) {             // insertion sort: a handful
        int64_t v = xs[i];
        int j = i - 1;
        while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
        xs[j + 1] = v;
    }
    return n & ~1;
}

void ugfx_fill_polygon(struct ugfx_surface *s, const int *xs, const int *ys,
                       int count, uint32_t color) {
    static struct polygon p;                   // 4 KiB: not a ring-3 frame's worth
    if (count < 3) return;
    if (count > POLY_MAX) count = POLY_MAX;
    int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
    for (int i = 0; i < count; i++) {
        p.x[i] = (int64_t)xs[i] * 256 + 128;
        p.y[i] = (int64_t)ys[i] * 256 + 128;
        if (p.x[i] < x0) x0 = p.x[i];
        if (p.x[i] > x1) x1 = p.x[i];
        if (p.y[i] < y0) y0 = p.y[i];
        if (p.y[i] > y1) y1 = p.y[i];
    }
    p.n = count;
    fill(s, x0, y0, x1, y1, poly_spans, &p, color);
}
