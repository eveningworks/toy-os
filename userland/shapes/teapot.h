// teapot.h -- the Utah teapot as a triangle mesh, for Shapes.
//
// Newell's 32 bicubic Bezier patches, tessellated ONCE into a
// TEAPOT_DIV x TEAPOT_DIV grid per patch -- positions and outward unit
// normals in the model's own frame, which is geom.h's: x right, y DOWN,
// z AWAY from the eye, centred on the teapot's bounding box so a yaw
// spins it on its own axis.
//
// The triangles are implicit in the grid: patch p's vertex (i, j) is
// teapot_vert(p, i, j), and each grid cell is the two triangles
// teapot_cell_tris() names, wound so a front-facing one has a POSITIVE
// screen-space signed area (x right, y down) -- culling is a sign test.
//
// No faces are shared between patches, so a seam vertex exists once per
// patch that touches it; with normals from the surface's own derivatives
// both copies shade alike and no seam shows.
#ifndef SHAPES_TEAPOT_H
#define SHAPES_TEAPOT_H

#include <stdint.h>
#include "geom.h"
#include "fixed.h"

#define TEAPOT_DIV     8
#define TEAPOT_PATCHES 32
#define TEAPOT_GRID    (TEAPOT_DIV + 1)
#define TEAPOT_VERTS   (TEAPOT_PATCHES * TEAPOT_GRID * TEAPOT_GRID)
#define TEAPOT_TRIS    (TEAPOT_PATCHES * TEAPOT_DIV * TEAPOT_DIV * 2)

struct teapot {
    struct geom_pt3 pos[TEAPOT_VERTS];
    struct geom_pt3 nrm[TEAPOT_VERTS];  // largest component +-1.0
    fx_t radius;                        // the farthest vertex from the centre
};

// Fill `t`. Deterministic, and allocates nothing.
void teapot_build(struct teapot *t);

static inline int teapot_vert(int patch, int i, int j) {
    return (patch * TEAPOT_GRID + i) * TEAPOT_GRID + j;
}

// Cell (i, j) of `patch` as two triangles, three vertex indices each.
static inline void teapot_cell_tris(int patch, int i, int j, int out[6]) {
    int a = teapot_vert(patch, i, j), b = teapot_vert(patch, i, j + 1);
    int c = teapot_vert(patch, i + 1, j), d = teapot_vert(patch, i + 1, j + 1);
    out[0] = a; out[1] = c; out[2] = d;
    out[3] = a; out[4] = d; out[5] = b;
}

#endif
