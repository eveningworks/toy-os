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

// A filled annulus SECTOR: everything between `r_inner` and `r_outer`,
// swept from `from` to `to`. This is what a ring gauge is made of, and
// a stack of concentric geom_ellipse() calls is not -- adjacent radii
// leave gaps the eye reads as moire.
//
// Swept as radial SPOKES rather than as scanlines, because a scanline
// has to be clipped against the angular limits and a spoke already is
// one. Sampled densely enough that neighbouring spokes touch at the
// OUTER edge, which is where they are furthest apart.
//
// ANGLES ARE TURNS (fixed.h), and turn 0 is at 3 o'clock with positive
// turns going CLOCKWISE on screen, because y grows downward. A gauge
// that wants to start at the top passes `from = -FX_ONE / 4`.
void geom_fill_ring(const struct geom_target *t, int cx, int cy,
                     int r_outer, int r_inner, fx_t from, fx_t to,
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

// --- 3D ---------------------------------------------------------------
//
// Enough to spin a wireframe MODEL and get 2D points back. Deliberately
// not a 3D engine: no matrices, no clipping planes, no depth buffer, no
// faces. A model here is points plus whatever edge list the caller keeps
// beside it, and what this file owns is the part every caller would
// otherwise re-derive identically -- three axis rotations and a
// perspective divide, in Q16.16, with angles in TURNS like everything
// else here.
//
// It lives in geom.c rather than in the one app that wanted it because
// this file IS the shared geometry home: it is compiled twice, so the
// kernel-space GUI can draw the same wireframe as a ring-3 client, and
// a second copy of a projection is a second thing that can disagree
// about where a vertex landed. (Noted honestly: CLAUDE.md's bar for a
// new API is a second real caller, and this shipped with one.)

struct geom_pt3 { fx_t x, y, z; };

// Rotation about each axis in turn -- yaw about Y, then pitch about X,
// then roll about Z. The order is fixed and stated because rotations do
// not commute: a caller composing its own would get a different result
// and have no way to know which one this file meant.
struct geom_pt3 geom_rotate3(struct geom_pt3 p, fx_t yaw, fx_t pitch, fx_t roll);

// Perspective projection onto the z = 0 plane, with the eye at
// (0, 0, -dist). A point's x and y shrink by dist / (dist + z), so
// nearer geometry is drawn larger -- which is the entire visual
// difference between a cube and a hexagon with a cross in it.
//
// `dist` is in the model's own units. Small values exaggerate the
// perspective; very large ones approach an orthographic projection,
// where a face-on cube collapses into a single square. A point at or
// behind the eye cannot be projected meaningfully, so the denominator
// is clamped to a small positive value rather than dividing by zero or
// flipping the point through the origin.
struct geom_pt geom_project(struct geom_pt3 p, fx_t dist);

// Scale, rotate, project and offset a whole model in one pass, writing
// rounded screen coordinates ready for geom_line()/geom_polyline().
//
// `out_z` (optional -- pass NULL) receives each point's ROTATED depth,
// before projection. That is what a caller needs to shade or sort edges
// by distance, and it is only available in here, which is why it is an
// output rather than something the caller recomputes.
void geom_transform3(const struct geom_pt3 *pts, int count,
                      fx_t yaw, fx_t pitch, fx_t roll, fx_t scale,
                      fx_t dist, int cx, int cy,
                      int *out_xs, int *out_ys, fx_t *out_z);

#endif
