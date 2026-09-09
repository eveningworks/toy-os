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

KTEST("geom", "a filled square covers exactly the block its corners name") {
    struct geom_target t = rec_target();
    int xs[4] = { 10, 20, 20, 10 }, ys[4] = { 10, 10, 20, 20 };
    geom_fill_polygon(&t, xs, ys, 4, 0xFFFFFF);
    KTEST_ASSERT(rec_has(10, 10));
    KTEST_ASSERT(rec_has(19, 19));
    KTEST_ASSERT(!rec_has(20, 15));     // the right edge is out...
    KTEST_ASSERT(!rec_has(15, 20));     // ...and so is the bottom one
    KTEST_ASSERT(!rec_has(9, 15));
    KTEST_ASSERT_EQ(g_rec.n, 100);      // every pixel once: no seam, no gap
}

KTEST("geom", "a filled polygon is even-odd, so a notch stays empty") {
    struct geom_target t = rec_target();
    // A U: 30 wide, 30 tall, with a 10-wide slot cut from the top.
    int xs[8] = { 0, 10, 10, 20, 20, 30, 30, 0 };
    int ys[8] = { 0,  0, 20, 20,  0,  0, 30, 30 };
    geom_fill_polygon(&t, xs, ys, 8, 0xFFFFFF);
    KTEST_ASSERT(rec_has(5, 5));        // the left arm
    KTEST_ASSERT(rec_has(25, 5));       // the right arm
    KTEST_ASSERT(rec_has(15, 25));      // the base
    KTEST_ASSERT(!rec_has(15, 5));      // the slot
    KTEST_ASSERT(!rec_has(15, 19));     // right down to its floor
    KTEST_ASSERT(rec_has(15, 20));      // which is where the base starts
}

KTEST("geom", "a polygon with too many vertices is refused, not overrun") {
    struct geom_target t = rec_target();
    int xs[GEOM_POLY_MAX + 1], ys[GEOM_POLY_MAX + 1];
    for (int i = 0; i <= GEOM_POLY_MAX; i++) { xs[i] = i; ys[i] = (i & 1) * 50; }
    geom_fill_polygon(&t, xs, ys, GEOM_POLY_MAX + 1, 0xFFFFFF);
    KTEST_ASSERT_EQ(g_rec.n, 0);
}

KTEST("geom", "a face normal points where the winding says") {
    // The front face of a cube 160 units across, wound counter-
    // clockwise on screen: the header promises a normal toward the eye
    // (negative z), and the size is the one that overflowed Q16.16
    // before the largest-component scaling.
    struct geom_pt3 a = { fx_from_int(-80), fx_from_int(-80), fx_from_int(-80) };
    struct geom_pt3 b = { fx_from_int(-80), fx_from_int( 80), fx_from_int(-80) };
    struct geom_pt3 c = { fx_from_int( 80), fx_from_int( 80), fx_from_int(-80) };
    struct geom_pt3 n = geom_face_normal3(a, b, c);
    KTEST_ASSERT_EQ(n.x, 0);
    KTEST_ASSERT_EQ(n.y, 0);
    KTEST_ASSERT_EQ(n.z, -FX_ONE);
    // The other winding is the other way.
    n = geom_face_normal3(a, c, b);
    KTEST_ASSERT_EQ(n.z, FX_ONE);
}

KTEST("geom", "Lambert: lit head-on is full, edge-on and turned away are dark") {
    struct geom_pt3 n = { 0, 0, -FX_ONE };
    struct geom_pt3 head_on = { 0, 0, -fx_from_int(7) };   // any length
    struct geom_pt3 edge_on = { FX_ONE, 0, 0 };
    struct geom_pt3 behind  = { 0, 0, FX_ONE };
    struct geom_pt3 oblique = { -FX_ONE, 0, -FX_ONE };     // 45 degrees off
    KTEST_ASSERT_EQ(geom_shade(n, head_on), 255);
    KTEST_ASSERT_EQ(geom_shade(n, edge_on), 0);
    KTEST_ASSERT_EQ(geom_shade(n, behind), 0);
    int s = geom_shade(n, oblique);                        // cos 45 = 0.707
    KTEST_ASSERT(s >= 179 && s <= 181);
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

// --- 3D ---------------------------------------------------------------
//
// Worth testing here rather than by looking at the Shapes demo for the
// reason this file opens with: a projection that is subtly wrong still
// produces a shape that spins convincingly. These pin the parts a
// picture cannot -- that the axes are the ones the header claims, that
// perspective actually makes near larger than far, and that a point at
// the eye does not divide by zero.

KTEST("geom", "each 3D rotation turns the axes the header says it does") {
    struct geom_pt3 p;

    // A quarter turn of YAW takes +x to -z: the x/z plane turns and y is
    // untouched. Getting a sign wrong here spins the model the wrong way
    // and looks entirely plausible.
    p = geom_rotate3((struct geom_pt3){ FX_ONE, 0, 0 }, FX_ONE / 4, 0, 0);
    KTEST_ASSERT_EQ(p.x, 0);
    KTEST_ASSERT_EQ(p.y, 0);
    KTEST_ASSERT_EQ(p.z, -FX_ONE);

    // A quarter turn of PITCH takes +y to +z, leaving x alone.
    p = geom_rotate3((struct geom_pt3){ 0, FX_ONE, 0 }, 0, FX_ONE / 4, 0);
    KTEST_ASSERT_EQ(p.x, 0);
    KTEST_ASSERT_EQ(p.y, 0);
    KTEST_ASSERT_EQ(p.z, FX_ONE);

    // A quarter turn of ROLL takes +x to +y, leaving z alone -- the same
    // turn the 2D geom_rotate() does, which is the point.
    p = geom_rotate3((struct geom_pt3){ FX_ONE, 0, 0 }, 0, 0, FX_ONE / 4);
    KTEST_ASSERT_EQ(p.x, 0);
    KTEST_ASSERT_EQ(p.y, FX_ONE);
    KTEST_ASSERT_EQ(p.z, 0);
}

KTEST("geom", "a zero rotation leaves a point exactly where it was") {
    struct geom_pt3 in = { 3 * FX_ONE, -7 * FX_ONE, 11 * FX_ONE };
    struct geom_pt3 p = geom_rotate3(in, 0, 0, 0);
    KTEST_ASSERT_EQ(p.x, in.x);
    KTEST_ASSERT_EQ(p.y, in.y);
    KTEST_ASSERT_EQ(p.z, in.z);
}

KTEST("geom", "perspective makes near bigger than far") {
    fx_t dist = fx_from_int(100);
    struct geom_pt3 p_near = { fx_from_int(10), 0, fx_from_int(-50) };
    struct geom_pt3 p_far  = { fx_from_int(10), 0, fx_from_int(50) };

    struct geom_pt n = geom_project(p_near, dist);
    struct geom_pt f = geom_project(p_far, dist);

    // THE property that distinguishes a cube from a flat hexagon: two
    // points at the same x, one nearer, must not project to the same
    // place. An orthographic projection -- or a divide that silently
    // cancelled -- passes every other check here.
    KTEST_ASSERT(n.x > f.x);
    KTEST_ASSERT_EQ(n.x, fx_from_int(20));  // 10 * 100/50
    KTEST_ASSERT(f.x < fx_from_int(10));    // 10 * 100/150

    // A point on the projection plane is unmoved by it.
    struct geom_pt on = geom_project((struct geom_pt3){ fx_from_int(7), fx_from_int(3), 0 }, dist);
    KTEST_ASSERT_EQ(on.x, fx_from_int(7));
    KTEST_ASSERT_EQ(on.y, fx_from_int(3));
}

KTEST("geom", "a point at or behind the eye clamps instead of dividing by zero") {
    fx_t dist = fx_from_int(100);

    // Exactly at the eye: the denominator is 0 before clamping.
    struct geom_pt at = geom_project((struct geom_pt3){ FX_ONE, 0, -dist }, dist);
    // Far off screen, which is the intended outcome -- not a crash, and
    // not a small plausible-looking coordinate.
    KTEST_ASSERT(at.x > fx_from_int(100));

    // Behind it: must NOT come back with a flipped sign, which is what
    // an unclamped divide produces and what makes a model turn inside
    // out at exactly one angle.
    struct geom_pt behind = geom_project((struct geom_pt3){ FX_ONE, 0, -2 * dist }, dist);
    KTEST_ASSERT(behind.x > 0);
}

KTEST("geom", "geom_transform3 reports the rotated depth it projected with") {
    // Two opposite corners of a cube, face on. With no rotation the
    // depths are simply the input z values -- which is the check that
    // out_z is the ROTATED depth and not something recomputed or the
    // pre-scale value.
    struct geom_pt3 pts[2] = {
        { fx_from_int(-10), 0, fx_from_int(-10) },
        { fx_from_int(-10), 0, fx_from_int(10) },
    };
    int xs[2], ys[2];
    fx_t z[2];
    geom_transform3(pts, 2, 0, 0, 0, FX_ONE, fx_from_int(100), 200, 100, xs, ys, z);

    KTEST_ASSERT_EQ(z[0], fx_from_int(-10));
    KTEST_ASSERT_EQ(z[1], fx_from_int(10));

    // Both are at x = -10 in the model, so the NEARER one must land
    // further from the centre. Same claim as the projection test, but
    // through the call an app actually makes.
    KTEST_ASSERT(xs[0] < xs[1]);
    KTEST_ASSERT(xs[1] < 200);
    KTEST_ASSERT_EQ(ys[0], 100);   // y = 0 stays on the centre line

    // Scale multiplies z as well as x and y -- a scale that only touched
    // two of the three axes would squash the model flat as it grew.
    fx_t z2[2];
    geom_transform3(pts, 2, 0, 0, 0, 2 * FX_ONE, fx_from_int(100), 200, 100, xs, ys, z2);
    KTEST_ASSERT_EQ(z2[1], fx_from_int(20));
}

// --- geom_fill_ring ---------------------------------------------------
//
// A gauge arc that LOOKS right is the exact failure this file exists to
// catch, so every check below is about a property a plausible-looking
// ring would still get wrong: the hole in the middle, the angular
// limits, and which way round the sweep goes.

// Is any plotted point inside the box [x0,x1] x [y0,y1]?
static int rec_any_in(int x0, int y0, int x1, int y1) {
    for (int i = 0; i < g_rec.n; i++) {
        if (g_rec.xs[i] >= x0 && g_rec.xs[i] <= x1 &&
            g_rec.ys[i] >= y0 && g_rec.ys[i] <= y1) return 1;
    }
    return 0;
}

KTEST("geom", "a full ring is an annulus -- ink in the band, none in the hole") {
    struct geom_target t = rec_target();
    geom_fill_ring(&t, 100, 100, 20, 12, 0, FX_ONE, 0xFFFFFF);
    KTEST_ASSERT(g_rec.n > 0);
    KTEST_ASSERT(!g_rec.overflow);

    // THE HOLE, asserted as the annulus invariant rather than as a box:
    // a box's CORNERS reach further from the centre than its sides, so
    // one sized by eye either misses ink or fails on a rounded spoke.
    // Every plotted point must sit in the band, one pixel of rounding
    // either side. A version that filled a disc passes every "is there
    // ink" check and fails this.
    for (int i = 0; i < g_rec.n; i++) {
        int dx = g_rec.xs[i] - 100, dy = g_rec.ys[i] - 100;
        KTEST_ASSERT(dx * dx + dy * dy >= 11 * 11);
    }
    KTEST_ASSERT(!rec_any_in(100, 100, 100, 100));   // and never the centre

    // ...and the band itself is inked on all four sides, so the sweep
    // really went the whole way round rather than stopping early.
    KTEST_ASSERT(rec_any_in(100 + 12, 100, 100 + 20, 100));   // 3 o'clock
    KTEST_ASSERT(rec_any_in(100 - 20, 100, 100 - 12, 100));   // 9 o'clock
    KTEST_ASSERT(rec_any_in(100, 100 + 12, 100, 100 + 20));   // 6 o'clock
    KTEST_ASSERT(rec_any_in(100, 100 - 20, 100, 100 - 12));   // 12 o'clock

    // ...and nothing outside the outer radius, which a spoke drawn one
    // step too far would produce.
    for (int i = 0; i < g_rec.n; i++) {
        int dx = g_rec.xs[i] - 100, dy = g_rec.ys[i] - 100;
        KTEST_ASSERT(dx * dx + dy * dy <= 21 * 21);
    }
}

KTEST("geom", "a quarter sweep inks one quadrant and leaves the rest bare") {
    // From 3 o'clock, a quarter turn CLOCKWISE on screen -- y grows
    // downward, so this is the lower-right quadrant. Getting the sign
    // wrong draws a perfectly good arc in the wrong place.
    struct geom_target t = rec_target();
    geom_fill_ring(&t, 100, 100, 20, 12, 0, FX_ONE / 4, 0xFFFFFF);
    KTEST_ASSERT(g_rec.n > 0);

    // Strictly BELOW the centre row: turn 0 inks y == 100 whichever way
    // the sweep goes, so a box touching that row cannot tell the two
    // directions apart. A reversed sweep passed this until it did not
    // include the shared boundary.
    KTEST_ASSERT(rec_any_in(100 + 4, 100 + 4, 100 + 20, 100 + 20));  // lower right
    KTEST_ASSERT(!rec_any_in(100 - 21, 100 - 21, 100 - 1, 100 - 1)); // upper left
    KTEST_ASSERT(!rec_any_in(100 - 21, 100 + 1, 100 - 1, 100 + 21)); // lower left
}

KTEST("geom", "a gauge starting at twelve o'clock fills clockwise from the top") {
    // What a ring gauge actually asks for: start a quarter turn BEFORE
    // 3 o'clock. A half sweep from there covers the right-hand side.
    struct geom_target t = rec_target();
    geom_fill_ring(&t, 100, 100, 20, 12, -FX_ONE / 4, FX_ONE / 4, 0xFFFFFF);

    KTEST_ASSERT(rec_any_in(100 + 12, 100 - 20, 100 + 20, 100 + 20)); // right half
    KTEST_ASSERT(!rec_any_in(100 - 21, 100 - 21, 100 - 13, 100 + 21)); // left half bare
}

KTEST("geom", "a degenerate ring draws nothing rather than something") {
    struct geom_target t = rec_target();
    geom_fill_ring(&t, 100, 100, 20, 12, 0, 0, 0xFFFFFF);   // zero sweep
    KTEST_ASSERT_EQ(g_rec.n, 0);

    t = rec_target();
    geom_fill_ring(&t, 100, 100, 12, 20, 0, FX_ONE, 0xFFFFFF); // inner > outer
    KTEST_ASSERT_EQ(g_rec.n, 0);

    // A ring with no hole IS a disc, and must still be filled -- this is
    // the boundary the "inner > outer" refusal must not swallow.
    t = rec_target();
    geom_fill_ring(&t, 100, 100, 6, 0, 0, FX_ONE, 0xFFFFFF);
    KTEST_ASSERT(rec_any_in(100, 100, 100, 100));
}
