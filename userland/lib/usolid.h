// A textured solid -- a cube, a pyramid or a ball -- that folds up out of
// one flat face, tumbles, and bounces around a screen.
//
// The mesh carries TEXTURE COORDINATES and the drawing goes through
// ui/ugfx_tex.h, so any picture can be wrapped round it; the Desktop cube
// saver wraps the desktop. The rotation and the lighting are api/geom.h's,
// and like geom.h the angles are in TURNS.
//
// **EVERY SHAPE IS BUILT AT A FOLD STAGE.** usolid_build() takes one
// progress value per hinge (usolid_stages() of them), 0 to FX_ONE: at all
// zeros the solid is nothing but its front face -- every other face is
// tucked behind it, wound away from the eye and so culled -- and at all
// FX_ONE it is closed. In between each face swings out on its hinge and
// folds back, which is what an intro animates. usolid_front() is that
// front face on its own, for whatever is about to shrink into it.
//
// NO DEPTH BUFFER: back faces are culled and the rest painted far to
// near, which is exact for a closed convex solid and close enough for one
// half folded (geom.h's painter's rule).
#ifndef ULIB_USOLID_H
#define ULIB_USOLID_H

#include <stdint.h>
#include "fixed.h"
#include "geom.h"
#include "ui/ugfx.h"
#include "ui/ugfx_tex.h"

enum usolid_shape { USOLID_CUBE = 0, USOLID_PYRAMID, USOLID_BALL, USOLID_SHAPES };

// The ball's grid, which is the largest mesh: 24 x 12 cells.
#define USOLID_VERT_MAX 325
#define USOLID_TRI_MAX  576
#define USOLID_STAGE_MAX 5

// One corner. `p` is in model units, the solid about the origin with a
// cube's half-edge as 1.0; `n` is read only for the ball, which is shaded
// per corner. u and v are texels, as ugfx_texvert's are.
struct usolid_vert {
    struct geom_pt3 p, n;
    int u, v;
};

// Where a corner landed, relative to the solid's centre on screen.
struct usolid_proj { int x, y, z, shade; };

struct usolid_box { int x0, y0, x1, y1; };

// The mesh AND the scratch drawing it needs, together, because a ring-3
// stack frame is capped at 2 KB and this is about 25: make it static.
struct usolid {
    int shape, smooth;
    int nv, nt;
    struct usolid_vert v[USOLID_VERT_MAX];
    uint16_t t[USOLID_TRI_MAX][3];
    // Filled by usolid_project(), read by usolid_draw().
    struct geom_pt3 rp[USOLID_VERT_MAX];
    struct usolid_proj pv[USOLID_VERT_MAX];
    uint8_t fshade[USOLID_TRI_MAX];   // a flat face's shade; 255 unlit
    uint16_t order[USOLID_TRI_MAX];
    int32_t key[USOLID_TRI_MAX];
};

// How many hinges `shape` folds on: the cube 5 (right, left, top, bottom,
// then the back, hinged on the right face), the pyramid 3 (one per edge
// of its front face), the ball 2 (the sheet rolls into a tube, then the
// tube's ends pinch into a sphere).
int usolid_stages(int shape);

// `stage` holds usolid_stages(shape) values, or is NULL for closed. `tw`
// and `th` are the texture's size: the cube and the ball put the whole
// picture on each face, the pyramid a triangle cut from its middle (apex
// top centre, base along the bottom).
void usolid_build(struct usolid *s, int shape, const fx_t *stage, int tw, int th);

// The front face at stage zero, as a quad TL, TR, BR, BL -- the pyramid's
// apex is TL and TR both. The same coordinates usolid_build() gives that
// face, so a flat picture shrunk onto it hands over without a seam.
void usolid_front(int shape, int tw, int th, struct usolid_vert q[4]);

struct usolid_view {
    fx_t yaw, pitch, roll;
    int unit;   // pixels per model unit
    int dist;   // the eye's distance from the centre, pixels; > 2 * unit
    int lit;    // 0: every face at full brightness
};

// Rotates and projects every corner into s->pv, and returns the bounding
// box of the result relative to the centre -- what a caller bounces with,
// before it decides where the centre goes.
void usolid_project(struct usolid *s, const struct usolid_view *v, struct usolid_box *box);

// One point through the same rotation and projection, for whatever is
// drawn to meet the solid -- the flat picture an intro shrinks onto it.
struct usolid_proj usolid_project_pt(const struct usolid_view *v, struct geom_pt3 p);

// Draws what usolid_project() last computed, centred on (cx, cy) and
// scaled about it by sx, sy (FX_ONE each for none -- a squash on impact).
void usolid_draw(struct ugfx_surface *surf, struct usolid *s, const struct ugfx_texture *tex,
                 int cx, int cy, fx_t sx, fx_t sy);

// --- moving it around a screen -----------------------------------------

// A centre and a velocity. The centre is in 1/256 px so a slow solid
// still moves on every frame.
struct usolid_motion {
    int x256, y256;
    int vx, vy;     // px per second
};

enum { USOLID_HIT_LEFT = 1, USOLID_HIT_RIGHT = 2, USOLID_HIT_TOP = 4, USOLID_HIT_BOTTOM = 8 };

// Advances by `dt_ms`, then keeps `box` (relative to the centre, as
// usolid_project() gives it) inside w x h: an edge it crossed sends it
// back the other way and is returned as a USOLID_HIT_* bit.
//
// THE BOX IS THE ONE JUST DRAWN, so it changes as the solid turns: a cube
// coming round onto a corner can grow into an edge without moving. That
// is pushed back out rather than counted as a second hit while it is
// already heading away.
int usolid_bounce(struct usolid_motion *m, const struct usolid_box *box,
                  int w, int h, int dt_ms);

#endif // ULIB_USOLID_H
