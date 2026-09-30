// ugfx_tex.h -- a perspective-correct textured triangle.
//
// **WHY THIS IS NOT IN geom.h.** geom.c is compiled TWICE, into the
// kernel and into ring 3, and may name nothing kernel-only -- it draws
// through a CALLBACK and never into a framebuffer (kernel/include/api/
// geom.h). Texturing is the opposite: it reads a source image per pixel
// and writes a destination surface, so it belongs where surfaces live.
// That is ugfx, which ships in libuapp.so -- every GUI app links it, so
// this is shared by existing.
//
// **PERSPECTIVE-CORRECT, AND THAT IS THE WHOLE POINT.** u and v are NOT
// linear in screen space; u/z, v/z and 1/z are. Interpolating u and v
// directly is affine mapping -- cheap, and the reason PlayStation 1
// textures visibly swim on a wall seen at an angle. The fix needs a
// divide to recover u and v from u/z and 1/z, and doing that per pixel
// is expensive without an FPU (there is none here: -mno-sse).
//
// So the divide is taken every UGFX_TEX_SPAN pixels and interpolated
// between, which is Quake's subdivided-span trick. The error inside a
// span is bounded by how much 1/z can change across 16 pixels, which at
// any sane field of view is invisible; the cost is one 64-bit divide per
// 16 pixels instead of per pixel.
#ifndef UGFX_TEX_H
#define UGFX_TEX_H

#include <stdint.h>
#include "ui/ugfx.h"

// How many pixels share one exact divide. A power of two so the
// step is a shift. 16 is Quake's number and the error at that length is
// below a pixel of texture for anything not nearly edge-on; drop it to
// 8 if a scene ever shows stepping, raise it for speed.
#define UGFX_TEX_SPAN 16

// One corner: where it landed on screen, and where it is in the texture.
//
// `z` IS THE VIEW DEPTH the projection already computed -- pass what
// geom_transform3() handed back for that corner, unchanged. It is used
// only as a ratio, so its units do not matter as long as every corner of
// a triangle uses the same ones, and it must be POSITIVE (in front of
// the eye); a caller that can produce a corner behind the eye has to
// clip before it gets here, which is the caller's business because the
// face list is (geom.h's rule).
struct ugfx_texvert {
    int x, y;      // screen, surface-local
    int z;         // view depth, > 0
    int u, v;      // texel, in TEXELS not fractions -- 0..tw-1, 0..th-1
    // 0..255, multiplying the colour at this corner; read ONLY by
    // ugfx_tri3d(), which interpolates it across the face (Gouraud).
    // ugfx_textured_tri() takes one shade for the whole face instead.
    int shade;
};

// The source image: 32bpp, no padding, the same shape as a surface's
// pixels. `w` and `h` need not be powers of two -- the wrap is a modulo
// the compiler turns into a mask when it can, and the cost of the
// general case is one instruction on a path that already divides.
struct ugfx_texture {
    const uint32_t *pixels;
    int w, h;
};

// Fill a triangle with `tex`, perspective-correct. Clipped to the
// surface's clip rect like every other ugfx primitive, and the dirty
// rect is grown to match.
//
// `shade` is 0..255 and multiplies every texel -- 255 leaves the texture
// alone. It is here rather than left to the caller because a lit
// textured face needs both and doing it in one pass avoids a second
// read of every pixel; it is also what lets a caller keep the Lambert
// term it already computes for the untextured path.
void ugfx_textured_tri(struct ugfx_surface *s, const struct ugfx_texture *tex,
                       const struct ugfx_texvert *a,
                       const struct ugfx_texvert *b,
                       const struct ugfx_texvert *c, int shade);

// A convenience for a QUAD given in order, which is what a cube face is:
// two triangles sharing the a-c diagonal, so a caller does not have to
// remember which diagonal keeps the winding consistent.
void ugfx_textured_quad(struct ugfx_surface *s, const struct ugfx_texture *tex,
                        const struct ugfx_texvert q[4], int shade);

// --- depth-tested triangles, for a mesh that is not convex -----------
//
// A DEPTH BUFFER, as GL and D3D keep one: per pixel, how near the
// nearest thing drawn there so far is. It holds 1/z rather than z,
// because 1/z is what interpolates LINEARLY across the screen (the same
// fact the texturing above rests on), so it needs no divide per pixel:
// LARGER IS NEARER and a cleared buffer is 0, "nothing yet". W-buffers
// and reverse-Z are the same idea on a GPU.
//
// It shadows a RECT of the surface -- a canvas, not the window -- and
// drawing through it is clipped to that rect as well as to the clip.
// The storage is the caller's (w * h entries), because its size follows
// the rect and the toolkit allocates nothing.
struct ugfx_zbuffer {
    uint32_t *depth;
    int x, y, w, h;   // surface-local
};

// Every entry to 0. Once per frame, before the first triangle.
void ugfx_zbuffer_clear(struct ugfx_zbuffer *zb);

// ONE triangle, as general as this file goes: each corner's `shade`
// interpolated across the face (Gouraud), the colour taken from `tex`
// perspective-correct or, with `tex` NULL, from `color`, and each pixel
// kept only if it is nearer than what `zb` already holds -- `zb` NULL
// skips the test and writes no depth, which is what a convex solid
// drawn front faces only needs. Winding does not matter; culling is
// the caller's, as the face list is (geom.h's rule).
void ugfx_tri3d(struct ugfx_surface *s, struct ugfx_zbuffer *zb,
                const struct ugfx_texture *tex, uint32_t color,
                const struct ugfx_texvert *a, const struct ugfx_texvert *b,
                const struct ugfx_texvert *c);

// Fill `px` (w*h) with a two-colour checkerboard of `cell`-pixel squares.
// Here rather than in an app because EVERY caller developing against
// this wants one: a checker is the pattern that makes a mapping error
// obvious, which a photograph hides.
void ugfx_texture_checker(uint32_t *px, int w, int h, int cell,
                          uint32_t a, uint32_t b);

#endif
