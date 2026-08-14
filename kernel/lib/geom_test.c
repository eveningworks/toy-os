// Tests for the shared geometry module and its fixed-point trig.
//
// These are worth having as KTESTs rather than eyeballing the demo: a
// rasteriser that is subtly wrong still LOOKS like a shape. A circle
// that is one pixel off-centre, a line that misses its endpoint, or a
// sine table with a sign error in one quadrant all render something
// perfectly plausible.
//
// The target here records what was plotted instead of drawing, which is
// the whole reason geom.c takes a plot callback -- it can be tested with
// no framebuffer, no window and no display driver at all.
#include "ktest.h"
#include "geom.h"
#include "fixed.h"

#define REC_MAX 8192

struct rec {
    int xs[REC_MAX];
    int ys[REC_MAX];
    uint8_t as[REC_MAX];
    int n;
    int overflow;
};

static struct rec g_rec;

static void rec_plot(void *ctx, int x, int y, uint32_t color, uint8_t alpha) {
    (void)ctx; (void)color;
    if (g_rec.n >= REC_MAX) { g_rec.overflow = 1; return; }
    g_rec.xs[g_rec.n] = x;
    g_rec.ys[g_rec.n] = y;
    g_rec.as[g_rec.n] = alpha;
    g_rec.n++;
}

static struct geom_target rec_target(void) {
    g_rec.n = 0;
    g_rec.overflow = 0;
    struct geom_target t = { rec_plot, 0 };
    return t;
}

static int rec_has(int x, int y) {
    for (int i = 0; i < g_rec.n; i++) if (g_rec.xs[i] == x && g_rec.ys[i] == y) return 1;
    return 0;
}

// Bounding box of everything plotted -- the cheap way to assert a shape
// is centred and sized correctly without listing every pixel.
static void rec_bounds(int *x0, int *y0, int *x1, int *y1) {
    *x0 = *y0 = 1 << 30;
    *x1 = *y1 = -(1 << 30);
    for (int i = 0; i < g_rec.n; i++) {
        if (g_rec.xs[i] < *x0) *x0 = g_rec.xs[i];
        if (g_rec.xs[i] > *x1) *x1 = g_rec.xs[i];
        if (g_rec.ys[i] < *y0) *y0 = g_rec.ys[i];
        if (g_rec.ys[i] > *y1) *y1 = g_rec.ys[i];
    }
}

// --- fixed point ------------------------------------------------------

KTEST("geom", "sin/cos hit their exact values at the quarter turns") {
    // Angles are in TURNS, so these are exact rather than approximate --
    // which is the argument for turns over radians (see fixed.h).
    KTEST_ASSERT_EQ(fx_sin(0), 0);
    KTEST_ASSERT_EQ(fx_sin(FX_ONE / 4), FX_ONE);   // sin(90 deg) = 1
    KTEST_ASSERT_EQ(fx_sin(FX_ONE / 2), 0);
    KTEST_ASSERT_EQ(fx_sin(3 * FX_ONE / 4), -FX_ONE);
    KTEST_ASSERT_EQ(fx_cos(0), FX_ONE);
    KTEST_ASSERT_EQ(fx_cos(FX_ONE / 4), 0);
    KTEST_ASSERT_EQ(fx_cos(FX_ONE / 2), -FX_ONE);
}

KTEST("geom", "an angle wraps instead of running off the table") {
    // A rotation counter increases forever; if wrapping were wrong this
    // would read past the table or mirror into the wrong quadrant.
    KTEST_ASSERT_EQ(fx_sin(FX_ONE), fx_sin(0));
    KTEST_ASSERT_EQ(fx_sin(FX_ONE * 5 + FX_ONE / 4), FX_ONE);
    KTEST_ASSERT_EQ(fx_sin(-FX_ONE + FX_ONE / 4), FX_ONE);
}

KTEST("geom", "sin stays within [-1, 1] across a whole turn") {
    // Catches a sign or interpolation error anywhere in the table,
    // which a spot check of the four cardinal angles would miss.
    for (int i = 0; i < 1024; i++) {
        fx_t a = (fx_t)(((int64_t)i << FX_SHIFT) / 1024);
        fx_t s = fx_sin(a);
        KTEST_ASSERT(s <= FX_ONE && s >= -FX_ONE);
    }
}

KTEST("geom", "a quarter-turn rotation maps +x onto +y") {
    struct geom_pt p = { FX_ONE, 0 };
    struct geom_pt r = geom_rotate(p, FX_ONE / 4);
    KTEST_ASSERT_EQ(r.x, 0);
    KTEST_ASSERT_EQ(r.y, FX_ONE);

    // ...and a full turn is the identity, which is the property a
    // long-running animation actually depends on.
    struct geom_pt back = geom_rotate(p, FX_ONE);
    KTEST_ASSERT_EQ(back.x, FX_ONE);
    KTEST_ASSERT_EQ(back.y, 0);
}

// --- lines ------------------------------------------------------------

KTEST("geom", "an aliased line hits both endpoints exactly") {
    struct geom_target t = rec_target();
    geom_line(&t, 10, 10, 40, 25, 0xFFFFFF, GEOM_ALIASED);
    KTEST_ASSERT(g_rec.n > 0);
    KTEST_ASSERT(rec_has(10, 10));
    KTEST_ASSERT(rec_has(40, 25));
    // Every pixel opaque -- the aliased path must never emit coverage.
    for (int i = 0; i < g_rec.n; i++) KTEST_ASSERT_EQ(g_rec.as[i], 255);
}

KTEST("geom", "a horizontal aliased line is exactly as long as asked") {
    struct geom_target t = rec_target();
    geom_line(&t, 5, 7, 15, 7, 0xFFFFFF, GEOM_ALIASED);
    KTEST_ASSERT_EQ(g_rec.n, 11); // inclusive of both ends
    for (int i = 0; i < g_rec.n; i++) KTEST_ASSERT_EQ(g_rec.ys[i], 7);
}

KTEST("geom", "an anti-aliased line emits partial coverage") {
    struct geom_target t = rec_target();
    geom_line(&t, 0, 0, 20, 7, 0xFFFFFF, GEOM_AA); // a shallow diagonal
    KTEST_ASSERT(g_rec.n > 0);

    // The point of AA: at least some pixels are PARTIAL. A run of
    // fully-opaque pixels would mean the coverage maths collapsed and
    // this is Bresenham wearing a different name.
    int partial = 0;
    for (int i = 0; i < g_rec.n; i++) if (g_rec.as[i] > 0 && g_rec.as[i] < 255) partial++;
    KTEST_ASSERT(partial > 0);
}

KTEST("geom", "a closed polyline draws the joining edge too") {
    int xs[3] = { 0, 10, 0 };
    int ys[3] = { 0, 0, 10 };

    struct geom_target t = rec_target();
    geom_polyline(&t, xs, ys, 3, 0, 0xFFFFFF, GEOM_ALIASED);
    int open_n = g_rec.n;

    t = rec_target();
    geom_polyline(&t, xs, ys, 3, 1, 0xFFFFFF, GEOM_ALIASED);
    KTEST_ASSERT(g_rec.n > open_n); // the third edge really was drawn
}

// --- curves -----------------------------------------------------------

KTEST("geom", "a circle is centred and the right size") {
    struct geom_target t = rec_target();
    geom_circle(&t, 100, 100, 30, 0xFFFFFF, GEOM_ALIASED);
    KTEST_ASSERT(g_rec.n > 0);
    KTEST_ASSERT(!g_rec.overflow);

    int x0, y0, x1, y1;
    rec_bounds(&x0, &y0, &x1, &y1);
    // One pixel of slack for rounding at the extremes; more than that
    // means the centre or the radius is wrong.
    KTEST_ASSERT(x0 >= 69 && x0 <= 71);
    KTEST_ASSERT(x1 >= 129 && x1 <= 131);
    KTEST_ASSERT(y0 >= 69 && y0 <= 71);
    KTEST_ASSERT(y1 >= 129 && y1 <= 131);
}

KTEST("geom", "an ellipse honours its two radii independently") {
    struct geom_target t = rec_target();
    geom_ellipse(&t, 200, 100, 60, 20, 0xFFFFFF, GEOM_ALIASED);
    int x0, y0, x1, y1;
    rec_bounds(&x0, &y0, &x1, &y1);

    // A circle-shaped bug (using one radius for both axes) is exactly
    // what this catches, and it is invisible in a screenshot of a
    // rotating shape.
    KTEST_ASSERT((x1 - x0) >= 118 && (x1 - x0) <= 122);
    KTEST_ASSERT((y1 - y0) >= 38 && (y1 - y0) <= 42);
}

KTEST("geom", "a curve has no gaps in it") {
    // Steps are chosen from the perimeter so samples land about a pixel
    // apart. If that maths were wrong the curve would be dotted -- which
    // reads as a rendering glitch and is easy to miss on a small radius.
    struct geom_target t = rec_target();
    geom_circle(&t, 60, 60, 40, 0xFFFFFF, GEOM_ALIASED);

    // Walk the right-hand quadrant: every row it spans must have been
    // touched at least once.
    for (int y = 25; y <= 95; y++) {
        int found = 0;
        for (int i = 0; i < g_rec.n && !found; i++) if (g_rec.ys[i] == y) found = 1;
        KTEST_ASSERT(found);
    }
}

KTEST("geom", "a filled ellipse covers its centre and stops at its edge") {
    struct geom_target t = rec_target();
    geom_fill_ellipse(&t, 50, 50, 20, 10, 0xFFFFFF);
    KTEST_ASSERT(rec_has(50, 50));      // the middle
    KTEST_ASSERT(rec_has(30, 50));      // the left extreme
    KTEST_ASSERT(rec_has(70, 50));      // the right extreme
    KTEST_ASSERT(!rec_has(71, 50));     // and not beyond it

    // A corner of the bounding box must be OUTSIDE the ellipse -- a
    // fill that quietly became a rectangle would pass every check that
    // only looks at the extremes.
    KTEST_ASSERT(!rec_has(70, 40));
}

KTEST("geom", "a degenerate shape is a point, not a crash") {
    struct geom_target t = rec_target();
    geom_circle(&t, 5, 5, 0, 0xFFFFFF, GEOM_ALIASED);
    KTEST_ASSERT(rec_has(5, 5));

    t = rec_target();
    geom_line(&t, 3, 3, 3, 3, 0xFFFFFF, GEOM_ALIASED);
    KTEST_ASSERT(rec_has(3, 3));

    // Negative radii are refused rather than looping toward a huge
    // step count.
    t = rec_target();
    geom_ellipse(&t, 0, 0, -5, -5, 0xFFFFFF, GEOM_ALIASED);
    KTEST_ASSERT_EQ(g_rec.n, 0);
}
