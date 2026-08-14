#ifndef GEOM_H
#define GEOM_H

#include <stdint.h>
#include "fixed.h"

// Geometry primitives: lines, polylines, circles, ellipses, and their
// filled forms. Everything this kernel's GUI could previously only
// approximate with rectangles.
//
// WHY IT TAKES A PLOT CALLBACK INSTEAD OF DRAWING
// -----------------------------------------------
// This file is compiled TWICE -- into the kernel (where it backs
// gfx_draw_line() and friends, writing to the framebuffer through the
// clip and damage machinery) and into ring-3 clients (where it backs
// ugfx_draw_line(), writing into a client's own window buffer). The two
// have nothing in common except "put a colour at a coordinate", so that
// is the entire interface between them.
//
// The alternative -- one copy per side -- would mean two Bresenhams and
// two ellipse rasterisers that have to agree, which is exactly the kind
// of duplication this project avoids elsewhere (see the shared
// arithmetic engine behind both Calculators). Geometry is pure integer
// maths with no hardware in it, so there is nothing to specialise.
//
// The cost is an indirect call per pixel. That is the same shape
// gfx_fill_rect() already has (it loops on gfx_put_pixel()), and it
// buys a single implementation of every algorithm here.

// `alpha` is coverage, 0..255. A caller that only draws opaque shapes
// can ignore it; the anti-aliased paths use it for partial pixels.
struct geom_target {
    void (*plot)(void *ctx, int x, int y, uint32_t color, uint8_t alpha);
    void *ctx;
};

// Anti-aliasing is per-call rather than a global mode, because the
// right answer differs WITHIN one drawing: a wireframe's diagonals want
// AA (jaggies crawl distractingly as a shape rotates), while a 1px
// window border wants a hard edge -- a blurred rectangle outline just
// looks out of focus.
enum geom_aa { GEOM_ALIASED = 0, GEOM_AA = 1 };

// --- lines ------------------------------------------------------------

// GEOM_ALIASED is Bresenham; GEOM_AA is Wu's algorithm, which plots two
// pixels per step with complementary coverage.
void geom_line(const struct geom_target *t, int x0, int y0, int x1, int y1,
                uint32_t color, enum geom_aa aa);

// `count` points, drawn end to end. `closed` joins the last back to the
// first -- which is what makes a polygon out of the same call, rather
// than a separate one that could disagree about the joining edge.
void geom_polyline(const struct geom_target *t, const int *xs, const int *ys,
                    int count, int closed, uint32_t color, enum geom_aa aa);

// --- curves -----------------------------------------------------------
//
// Circles and ellipses share one implementation: a circle IS an ellipse
// with rx == ry, and keeping a separate midpoint-circle rasteriser
// would be a second thing to keep in agreement for no visual gain.
//
// Both are traced PARAMETRICALLY, stepping an angle with fx_sin/fx_cos
// rather than by the classic midpoint-error method. Two reasons: the
// step count can be chosen from the perimeter so there are never gaps,
// and the same sub-pixel positions that make the anti-aliased version
// smooth fall out of it directly. The trig it needs already had to
// exist for rotation.

void geom_ellipse(const struct geom_target *t, int cx, int cy, int rx, int ry,
                   uint32_t color, enum geom_aa aa);

void geom_circle(const struct geom_target *t, int cx, int cy, int r,
                  uint32_t color, enum geom_aa aa);

// Filled forms are scanline-based (a span per row), not "draw the
// outline then flood" -- flooding needs somewhere to read pixels back
// from, which a plot-only target deliberately does not have.
void geom_fill_ellipse(const struct geom_target *t, int cx, int cy, int rx, int ry,
                        uint32_t color);

void geom_fill_circle(const struct geom_target *t, int cx, int cy, int r,
                       uint32_t color);

// --- 2D transforms ----------------------------------------------------
//
// The minimum needed to rotate a shape about a point, which is what
// every "spinning wireframe" wants and what no caller should have to
// re-derive. Angles are in TURNS (see fixed.h).

struct geom_pt { fx_t x, y; };

// Rotates `p` about the origin by `turns`.
struct geom_pt geom_rotate(struct geom_pt p, fx_t turns);

// Rotates every point of a shape about (cx, cy) and writes the rounded
// results into `out_xs`/`out_ys`, ready for geom_polyline(). Scaling is
// applied first, in Q16.16, so a caller can spin and pulse a shape with
// one call rather than composing transforms by hand.
void geom_transform(const struct geom_pt *pts, int count,
                     fx_t turns, fx_t scale, int cx, int cy,
                     int *out_xs, int *out_ys);

#endif
