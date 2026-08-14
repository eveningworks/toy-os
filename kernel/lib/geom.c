// See geom.h for the design and why this takes a plot callback.
#include "geom.h"

static void put(const struct geom_target *t, int x, int y, uint32_t c, uint8_t a) {
    if (a == 0) return; // fully transparent -- never worth a call
    t->plot(t->ctx, x, y, c, a);
}

// Integer square root, bit by bit. Needed by the filled forms to solve
// the ellipse equation per scanline; there is no libm here and no FPU
// available to the kernel half of this file.
static uint32_t isqrt32(uint32_t n) {
    uint32_t root = 0, rem = n, place = 1u << 30;
    while (place > rem) place >>= 2;
    while (place) {
        if (rem >= root + place) {
            rem -= root + place;
            root += place << 1;
        }
        root >>= 1;
        place >>= 2;
    }
    return root;
}

// --- lines ------------------------------------------------------------

static void line_bresenham(const struct geom_target *t, int x0, int y0, int x1, int y1,
                            uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1;
    int sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;
    for (;;) {
        put(t, x0, y0, color, 255);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// Wu's algorithm: step along the major axis and plot the two pixels
// straddling the true line, with coverage split between them. That
// second pixel is the whole difference between this and Bresenham, and
// it is what stops a rotating shape's edges crawling.
static void line_wu(const struct geom_target *t, int x0, int y0, int x1, int y1,
                     uint32_t color) {
    int steep = (y1 > y0 ? y1 - y0 : y0 - y1) > (x1 > x0 ? x1 - x0 : x0 - x1);
    if (steep) { int tmp; tmp = x0; x0 = y0; y0 = tmp; tmp = x1; x1 = y1; y1 = tmp; }
    if (x0 > x1) { int tmp; tmp = x0; x0 = x1; x1 = tmp; tmp = y0; y0 = y1; y1 = tmp; }

    int dx = x1 - x0;
    int dy = y1 - y0;
    if (dx == 0) { // a single point, or a vertical line after the swap
        for (int y = (y0 < y1 ? y0 : y1); y <= (y0 < y1 ? y1 : y0); y++) {
            if (steep) put(t, y, x0, color, 255);
            else       put(t, x0, y, color, 255);
        }
        return;
    }

    fx_t gradient = fx_div(fx_from_int(dy), fx_from_int(dx));
    fx_t inter = fx_from_int(y0);

    for (int x = x0; x <= x1; x++) {
        int iy = inter >> FX_SHIFT;
        // Negative coordinates floor toward zero with a shift, which is
        // what we want here: fx_frac() is always non-negative, so the
        // pair (iy, iy+1) still brackets the true position.
        fx_t f = fx_frac(inter);
        uint8_t a2 = (uint8_t)(f >> 8);          // fractional part, 0..255
        uint8_t a1 = (uint8_t)(255 - a2);

        if (steep) {
            put(t, iy,     x, color, a1);
            put(t, iy + 1, x, color, a2);
        } else {
            put(t, x, iy,     color, a1);
            put(t, x, iy + 1, color, a2);
        }
        inter += gradient;
    }
}

void geom_line(const struct geom_target *t, int x0, int y0, int x1, int y1,
                uint32_t color, enum geom_aa aa) {
    if (!t || !t->plot) return;
    if (aa == GEOM_AA) line_wu(t, x0, y0, x1, y1, color);
    else               line_bresenham(t, x0, y0, x1, y1, color);
}

void geom_polyline(const struct geom_target *t, const int *xs, const int *ys,
                    int count, int closed, uint32_t color, enum geom_aa aa) {
    if (!t || !xs || !ys || count < 2) return;
    for (int i = 0; i + 1 < count; i++) {
        geom_line(t, xs[i], ys[i], xs[i + 1], ys[i + 1], color, aa);
    }
    if (closed) geom_line(t, xs[count - 1], ys[count - 1], xs[0], ys[0], color, aa);
}

// --- curves -----------------------------------------------------------

// Splats a sub-pixel point across the four pixels it straddles, with
// coverage weighted by how far into each it falls. This is what makes
// the parametric trace look like a smooth curve rather than a dotted
// one -- the samples land between pixels far more often than on them.
static void splat(const struct geom_target *t, fx_t x, fx_t y, uint32_t color) {
    int ix = x >> FX_SHIFT, iy = y >> FX_SHIFT;
    fx_t fx = fx_frac(x), fy = fx_frac(y);
    fx_t rx = FX_ONE - fx, ry = FX_ONE - fy;

    put(t, ix,     iy,     color, (uint8_t)(fx_mul(rx, ry) >> 8));
    put(t, ix + 1, iy,     color, (uint8_t)(fx_mul(fx, ry) >> 8));
    put(t, ix,     iy + 1, color, (uint8_t)(fx_mul(rx, fy) >> 8));
    put(t, ix + 1, iy + 1, color, (uint8_t)(fx_mul(fx, fy) >> 8));
}

// Step count chosen so consecutive samples land about a pixel apart:
// the perimeter of an ellipse is roughly pi * (rx + ry), so 3 * (rx+ry)
// is a hair over one sample per pixel. Too few leaves gaps; far too
// many just re-blends the same pixels and thickens the curve.
static int ellipse_steps(int rx, int ry) {
    int n = 3 * (rx + ry) + 8;
    if (n < 24) n = 24;
    if (n > 4096) n = 4096;
    return n;
}

void geom_ellipse(const struct geom_target *t, int cx, int cy, int rx, int ry,
                   uint32_t color, enum geom_aa aa) {
    if (!t || !t->plot || rx < 0 || ry < 0) return;
    if (rx == 0 && ry == 0) { put(t, cx, cy, color, 255); return; }

    int steps = ellipse_steps(rx, ry);
    fx_t fcx = fx_from_int(cx), fcy = fx_from_int(cy);
    fx_t frx = fx_from_int(rx), fry = fx_from_int(ry);

    for (int i = 0; i < steps; i++) {
        // The angle is (i / steps) of a turn, computed in fixed point
        // without ever forming a ratio > 1.
        fx_t turns = (fx_t)(((int64_t)i << FX_SHIFT) / steps);
        fx_t x = fcx + fx_mul(frx, fx_cos(turns));
        fx_t y = fcy + fx_mul(fry, fx_sin(turns));

        if (aa == GEOM_AA) splat(t, x, y, color);
        else               put(t, fx_round(x), fx_round(y), color, 255);
    }
}

void geom_circle(const struct geom_target *t, int cx, int cy, int r,
                  uint32_t color, enum geom_aa aa) {
    geom_ellipse(t, cx, cy, r, r, color, aa);
}

void geom_fill_ellipse(const struct geom_target *t, int cx, int cy, int rx, int ry,
                        uint32_t color) {
    if (!t || !t->plot || rx < 0 || ry < 0) return;

    // One horizontal span per row, with the half-width from the ellipse
    // equation: (x/rx)^2 + (y/ry)^2 = 1, so x = rx * sqrt(1 - (y/ry)^2).
    // Rearranged to stay in integers: x = rx * sqrt(ry^2 - y^2) / ry.
    for (int dy = -ry; dy <= ry; dy++) {
        int64_t inside = (int64_t)ry * ry - (int64_t)dy * dy;
        if (inside < 0) continue;
        int half = ry > 0 ? (int)(((int64_t)rx * isqrt32((uint32_t)inside)) / ry) : rx;
        for (int x = cx - half; x <= cx + half; x++) put(t, x, cy + dy, color, 255);
    }
}

void geom_fill_circle(const struct geom_target *t, int cx, int cy, int r,
                       uint32_t color) {
    geom_fill_ellipse(t, cx, cy, r, r, color);
}

// --- transforms -------------------------------------------------------

struct geom_pt geom_rotate(struct geom_pt p, fx_t turns) {
    fx_t s = fx_sin(turns), c = fx_cos(turns);
    struct geom_pt r;
    r.x = fx_mul(p.x, c) - fx_mul(p.y, s);
    r.y = fx_mul(p.x, s) + fx_mul(p.y, c);
    return r;
}

void geom_transform(const struct geom_pt *pts, int count,
                     fx_t turns, fx_t scale, int cx, int cy,
                     int *out_xs, int *out_ys) {
    if (!pts || !out_xs || !out_ys) return;
    for (int i = 0; i < count; i++) {
        struct geom_pt p = pts[i];
        // Scale BEFORE rotating. The other order gives the same result
        // for a uniform scale, but doing it consistently means a
        // non-uniform one later does not silently change behaviour.
        p.x = fx_mul(p.x, scale);
        p.y = fx_mul(p.y, scale);
        p = geom_rotate(p, turns);
        out_xs[i] = cx + fx_round(p.x);
        out_ys[i] = cy + fx_round(p.y);
    }
}
