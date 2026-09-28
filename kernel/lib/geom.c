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
        int e2 = err * 2;   // err goes negative; << on it is UB
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

void geom_fill_polygon(const struct geom_target *t, const int *xs, const int *ys,
                        int count, uint32_t color) {
    if (!t || !t->plot || !xs || !ys || count < 3 || count > GEOM_POLY_MAX) return;

    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < count; i++) {
        if (ys[i] < ymin) ymin = ys[i];
        if (ys[i] > ymax) ymax = ys[i];
    }

    for (int y = ymin; y < ymax; y++) {
        // Where each edge crosses this row's centre line, Q16.16. An
        // edge counts on [min(y0,y1), max(y0,y1)) -- half-open, so a
        // vertex shared by two edges is crossed once, not twice.
        fx_t xc[GEOM_POLY_MAX];
        int n = 0;
        fx_t ysample = fx_from_int(y) + FX_HALF;
        for (int i = 0; i < count; i++) {
            int j = (i + 1) % count;
            int y0 = ys[i], y1 = ys[j];
            if (y0 == y1) continue;
            int lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
            if (ysample < fx_from_int(lo) || ysample >= fx_from_int(hi)) continue;
            int64_t num = (int64_t)(ysample - fx_from_int(y0)) * (xs[j] - xs[i]);
            xc[n++] = fx_from_int(xs[i]) + (fx_t)(num / (y1 - y0));
        }
        for (int i = 1; i < n; i++) {          // insertion sort: n is tiny
            fx_t v = xc[i];
            int k = i;
            while (k > 0 && xc[k - 1] > v) { xc[k] = xc[k - 1]; k--; }
            xc[k] = v;
        }
        // Pixel x is inside when its centre lies in [xa, xb): the first
        // such x is ceil(xa - 0.5), the last is ceil(xb - 0.5) - 1.
        for (int i = 0; i + 1 < n; i += 2) {
            int xa = fx_to_int(xc[i] - FX_HALF + FX_ONE - 1);
            int xb = fx_to_int(xc[i + 1] - FX_HALF + FX_ONE - 1);
            for (int x = xa; x < xb; x++) put(t, x, y, color, 255);
        }
    }
}

void geom_fill_circle(const struct geom_target *t, int cx, int cy, int r,
                       uint32_t color) {
    geom_fill_ellipse(t, cx, cy, r, r, color);
}
void geom_fill_ring(const struct geom_target *t, int cx, int cy,
                     int r_outer, int r_inner, fx_t from, fx_t to,
                     uint32_t color) {
    if (!t || !t->plot || r_outer < 0) return;
    if (r_inner < 0) r_inner = 0;
    if (r_inner > r_outer) return;

    fx_t sweep = to - from;
    if (sweep <= 0) return;
    if (sweep > FX_ONE) sweep = FX_ONE;   // more than a full turn is a full turn

    // One full turn is 2*pi*r_outer pixels around, so eight samples per
    // unit of radius over-samples it comfortably; the +16 keeps a tiny
    // ring from being drawn as a polygon.
    int64_t steps = 8 * (int64_t)r_outer + 16;
    if (steps > 32768) steps = 32768;
    int64_t n = (steps * sweep) / FX_ONE;
    if (n < 1) n = 1;

    for (int64_t i = 0; i <= n; i++) {
        fx_t turns = from + (fx_t)(((int64_t)sweep * i) / n);
        fx_t c = fx_cos(turns), sn = fx_sin(turns);
        for (int r = r_inner; r <= r_outer; r++) {
            fx_t fr = fx_from_int(r);
            put(t, cx + fx_round(fx_mul(fr, c)),
                   cy + fx_round(fx_mul(fr, sn)), color, 255);
        }
    }
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

// --- 3D ---------------------------------------------------------------

struct geom_pt3 geom_rotate3(struct geom_pt3 p, fx_t yaw, fx_t pitch, fx_t roll) {
    fx_t s, c;
    struct geom_pt3 r;

    // Yaw, about the Y axis: x and z turn, y is untouched.
    s = fx_sin(yaw); c = fx_cos(yaw);
    r.x = fx_mul(p.x, c) + fx_mul(p.z, s);
    r.y = p.y;
    r.z = fx_mul(p.z, c) - fx_mul(p.x, s);
    p = r;

    // Pitch, about X.
    s = fx_sin(pitch); c = fx_cos(pitch);
    r.x = p.x;
    r.y = fx_mul(p.y, c) - fx_mul(p.z, s);
    r.z = fx_mul(p.y, s) + fx_mul(p.z, c);
    p = r;

    // Roll, about Z -- the same two-axis turn geom_rotate() does, kept
    // here rather than delegating because the 2D version takes a
    // geom_pt and building one per call to save three lines would cost
    // more than it saves.
    s = fx_sin(roll); c = fx_cos(roll);
    r.x = fx_mul(p.x, c) - fx_mul(p.y, s);
    r.y = fx_mul(p.x, s) + fx_mul(p.y, c);
    r.z = p.z;
    return r;
}

// The smallest denominator the projection will divide by, in Q16.16 --
// a quarter of a model unit. A point at exactly the eye has no
// projection at all, and a point BEHIND it has one that is mathematically
// valid and visually nonsense (the shape turns inside out through the
// origin). Clamping puts such a point far off screen instead, where the
// caller's own clipping deals with it, rather than producing a
// plausible-looking wrong coordinate.
#define PROJ_MIN_DEN (FX_ONE / 4)

struct geom_pt geom_project(struct geom_pt3 p, fx_t dist) {
    fx_t den = dist + p.z;
    if (den < PROJ_MIN_DEN) den = PROJ_MIN_DEN;

    fx_t k = fx_div(dist, den);
    struct geom_pt out;
    out.x = fx_mul(p.x, k);
    out.y = fx_mul(p.y, k);
    return out;
}

void geom_transform3(const struct geom_pt3 *pts, int count,
                      fx_t yaw, fx_t pitch, fx_t roll, fx_t scale,
                      fx_t dist, int cx, int cy,
                      int *out_xs, int *out_ys, fx_t *out_z) {
    if (!pts || !out_xs || !out_ys) return;
    for (int i = 0; i < count; i++) {
        struct geom_pt3 p = pts[i];
        // Scale first, then rotate, then project -- the same order
        // geom_transform() uses, extended by the one step it lacks.
        p.x = fx_mul(p.x, scale);
        p.y = fx_mul(p.y, scale);
        p.z = fx_mul(p.z, scale);
        p = geom_rotate3(p, yaw, pitch, roll);
        if (out_z) out_z[i] = p.z;

        struct geom_pt q = geom_project(p, dist);
        out_xs[i] = cx + fx_round(q.x);
        out_ys[i] = cy + fx_round(q.y);
    }
}

// Largest-component scaling, shared by the normal and the shade: the
// cross product of two 160-unit edges is 25600 units, which is already
// past what Q16.16 holds, so the direction is kept and the length is
// not. Vectors are scaled in int64 and only then narrowed.
static struct geom_pt3 unit_by_max(int64_t x, int64_t y, int64_t z) {
    int64_t m = x < 0 ? -x : x;
    int64_t ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
    if (ay > m) m = ay;
    if (az > m) m = az;
    struct geom_pt3 r = { 0, 0, 0 };
    if (m == 0) return r;
    r.x = (fx_t)(x * FX_ONE / m);
    r.y = (fx_t)(y * FX_ONE / m);
    r.z = (fx_t)(z * FX_ONE / m);
    return r;
}

struct geom_pt3 geom_face_normal3(struct geom_pt3 a, struct geom_pt3 b,
                                   struct geom_pt3 c) {
    int64_t ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    int64_t vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    return unit_by_max(uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx);
}

static uint64_t isqrt64(uint64_t n) {
    uint64_t root = 0, rem = n, place = 1ull << 62;
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

int geom_shade(struct geom_pt3 n, struct geom_pt3 light) {
    // Both scaled to |component| <= 1.0 first, so every product below
    // fits: a term is at most 2^32 and a sum of three at most 3 * 2^32.
    n = unit_by_max(n.x, n.y, n.z);
    light = unit_by_max(light.x, light.y, light.z);
    int64_t dot = (int64_t)n.x * light.x + (int64_t)n.y * light.y + (int64_t)n.z * light.z;
    if (dot <= 0) return 0;
    uint64_t nn = (uint64_t)((int64_t)n.x * n.x + (int64_t)n.y * n.y + (int64_t)n.z * n.z);
    uint64_t ll = (uint64_t)((int64_t)light.x * light.x + (int64_t)light.y * light.y +
                             (int64_t)light.z * light.z);
    // sqrt(nn) * sqrt(ll), never sqrt(nn * ll): the product of the two
    // squares does not fit in 64 bits, the product of the roots does.
    int64_t den = (int64_t)(isqrt64(nn) * isqrt64(ll));
    if (den <= 0) return 0;
    int64_t shade = (dot * 255 + den / 2) / den;
    return shade > 255 ? 255 : (int)shade;
}
