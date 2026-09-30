// Perspective-correct textured triangles -- see ui/ugfx_tex.h for why
// this lives in ugfx rather than in geom, and what the span subdivision
// is for.
#include "ui/ugfx_tex.h"
#include <stddef.h>   // size_t: the row arithmetic below
#include <string.h>   // memset: clearing a depth buffer

// 16.16, matching fixed.h's shift so a reader is not converting between
// two fixed formats in their head. Kept local because these are SCREEN
// interpolants, not angles -- fixed.h's fx_t carries turns and its
// helpers wrap, which is exactly wrong here.
#define FRAC 16

// **"INACTIVE" IS NOT "EMPTY".** A surface with no clip set has
// clip_x0..clip_y1 all ZERO, and reading them as bounds makes every
// draw a no-op -- which is exactly what happened the first time this
// ran: a black canvas and a toggle that reported itself on. ugfx.h says
// not to poke those fields for this reason; this is the one place that
// has to, because it writes pixels itself rather than through a call
// that would have resolved them.
struct clipbox { int x0, y0, x1, y1; };

static struct clipbox clip_of(const struct ugfx_surface *s) {
    struct clipbox c = { 0, 0, s->w, s->h };
    if (s->clip_active) {
        c.x0 = s->clip_x0; c.y0 = s->clip_y0;
        c.x1 = s->clip_x1; c.y1 = s->clip_y1;
    }
    return c;
}

static inline uint32_t shade_px(uint32_t c, int k) {
    if (k >= 255) return c;
    unsigned r = ((c >> 16) & 0xFF) * (unsigned)k / 255;
    unsigned g = ((c >> 8) & 0xFF) * (unsigned)k / 255;
    unsigned b = (c & 0xFF) * (unsigned)k / 255;
    return (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

// What one edge -- or one pixel -- carries. Everything is in the OVER-Z
// domain: uz, vz and iz are linear in screen space, and u and v are
// recovered from them by a divide. That is the whole reason this looks
// right and affine does not. `sh` is the Gouraud shade, 16.16, linear
// in screen space as Gouraud's is.
struct interp { int64_t uz, vz, iz, sh; };

struct tri_ctx {
    struct ugfx_surface *s;
    struct ugfx_zbuffer *zb;
    const struct ugfx_texture *tex;   // NULL: `color`
    uint32_t color;
    struct clipbox cb;
};

// One horizontal run at `y`, between two already-interpolated ends.
static void span(const struct tri_ctx *c, int y, int xl, int xr,
                 struct interp l, struct interp r) {
    if (xr <= xl) return;
    if (y < c->cb.y0 || y >= c->cb.y1) return;

    int w = xr - xl;
    int64_t duz = (r.uz - l.uz) / w, dvz = (r.vz - l.vz) / w;
    int64_t dsh = (r.sh - l.sh) / w;
    // Depth steps with FRAC extra bits: a per-pixel step truncated to an
    // integer drifts by up to a unit per pixel across the span, which is
    // the same order as the gap between two nearby surfaces -- and the
    // symptom is the far one poking through in stripes.
    int64_t izf = l.iz << FRAC, dizf = ((r.iz - l.iz) << FRAC) / w;
    int64_t diz = (r.iz - l.iz) / w;

    // CLIPPED BY STEPPING THE INTERPOLANTS, not by starting the loop
    // late with stale values: skipping x without advancing them maps
    // the wrong texel to every pixel of a partially off-screen face,
    // and it only shows when a face touches an edge.
    if (xl < c->cb.x0) {
        int skip = c->cb.x0 - xl;
        l.uz += duz * skip; l.vz += dvz * skip; l.iz += diz * skip;
        l.sh += dsh * skip; izf += dizf * skip;
        xl = c->cb.x0;
    }
    if (xr > c->cb.x1) xr = c->cb.x1;
    if (xr <= xl) return;

    struct ugfx_surface *s = c->s;
    const struct ugfx_texture *tex = c->tex;
    uint32_t *row = s->pixels + (size_t)y * s->w;
    // Indexed from the buffer's own origin: `depth - zb->x` would point
    // before the array on its first row, which C does not allow.
    uint32_t *zrow = c->zb ? c->zb->depth + (size_t)(y - c->zb->y) * c->zb->w : 0;
    int zx = c->zb ? c->zb->x : 0;
    int64_t sh = l.sh;
    int x = xl;
    while (x < xr) {
        int n = UGFX_TEX_SPAN;
        if (x + n > xr) n = xr - x;

        // THE EXACT DIVIDE, once per span, at both ends of it -- and only
        // when there is a texture to look up.
        int u = 0, v = 0, du = 0, dv = 0;
        if (tex) {
            int64_t iz0 = l.iz, iz1 = l.iz + diz * n;
            int u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            if (iz0 > 0) { u0 = (int)((l.uz << FRAC) / iz0); v0 = (int)((l.vz << FRAC) / iz0); }
            if (iz1 > 0) {
                u1 = (int)(((l.uz + duz * n) << FRAC) / iz1);
                v1 = (int)(((l.vz + dvz * n) << FRAC) / iz1);
            }
            // ...and a plain linear walk between them, which is the saving.
            du = n ? (u1 - u0) / n : 0; dv = n ? (v1 - v0) / n : 0;
            u = u0; v = v0;
        }

        for (int i = 0; i < n; i++, x++) {
            int k = (int)(sh >> FRAC);
            sh += dsh;
            uint32_t d = (uint32_t)(izf >> FRAC);
            izf += dizf;
            int pu = u, pv = v;
            u += du; v += dv;
            if (zrow) {
                if (d <= zrow[x - zx]) continue;   // something nearer is already here
                zrow[x - zx] = d;
            }
            uint32_t px = c->color;
            if (tex) {
                int tu = (pu >> FRAC) % tex->w, tv = (pv >> FRAC) % tex->h;
                if (tu < 0) tu += tex->w;
                if (tv < 0) tv += tex->h;
                px = tex->pixels[(size_t)tv * tex->w + tu];
            }
            row[x] = shade_px(px, k < 0 ? 0 : k);
        }
        l.uz += duz * n; l.vz += dvz * n; l.iz += diz * n;
    }
    ugfx_mark_dirty_rect(s, xl, y, xr - xl, 1);
}

static struct interp lerp(struct interp a, struct interp b, int f) {
    struct interp r = {
        a.uz + ((b.uz - a.uz) * f >> FRAC), a.vz + ((b.vz - a.vz) * f >> FRAC),
        a.iz + ((b.iz - a.iz) * f >> FRAC), a.sh + ((b.sh - a.sh) * f >> FRAC),
    };
    return r;
}

void ugfx_zbuffer_clear(struct ugfx_zbuffer *zb) {
    if (zb && zb->depth && zb->w > 0 && zb->h > 0)
        memset(zb->depth, 0, (size_t)zb->w * zb->h * sizeof zb->depth[0]);
}

void ugfx_tri3d(struct ugfx_surface *s, struct ugfx_zbuffer *zb,
                const struct ugfx_texture *tex, uint32_t color,
                const struct ugfx_texvert *a, const struct ugfx_texvert *b,
                const struct ugfx_texvert *c) {
    if (!s || !s->pixels) return;
    if (tex && (!tex->pixels || tex->w <= 0 || tex->h <= 0)) return;
    if (zb && (!zb->depth || zb->w <= 0 || zb->h <= 0)) return;
    if (a->z <= 0 || b->z <= 0 || c->z <= 0) return;   // behind the eye: caller's clip

    // Sorted top to bottom, so the sweep below has one shape rather than
    // three cases.
    const struct ugfx_texvert *t = a, *m = b, *l = c, *tmp;
    if (t->y > m->y) { tmp = t; t = m; m = tmp; }
    if (m->y > l->y) { tmp = m; m = l; l = tmp; }
    if (t->y > m->y) { tmp = t; t = m; m = tmp; }
    if (l->y == t->y) return;   // zero height

    // `iz` is scaled up so that small depth differences survive the
    // integer divide -- it is also what the depth buffer compares, so
    // the scale is the depth precision. 2^30 over a z of at least 1
    // still fits the buffer's 32 bits.
    #define IZ_SCALE (1 << 30)
    struct interp at[3];
    const struct ugfx_texvert *vtx[3] = { t, m, l };
    for (int i = 0; i < 3; i++) {
        at[i].iz = (int64_t)IZ_SCALE / vtx[i]->z;
        at[i].uz = (int64_t)vtx[i]->u * at[i].iz;
        at[i].vz = (int64_t)vtx[i]->v * at[i].iz;
        at[i].sh = (int64_t)vtx[i]->shade << FRAC;
    }
    #undef IZ_SCALE

    struct tri_ctx ctx = { s, zb, tex, color, clip_of(s) };
    if (zb) {
        // The depth buffer's rect bounds the drawing too: there is no
        // depth to test against outside it.
        if (ctx.cb.x0 < zb->x) ctx.cb.x0 = zb->x;
        if (ctx.cb.y0 < zb->y) ctx.cb.y0 = zb->y;
        if (ctx.cb.x1 > zb->x + zb->w) ctx.cb.x1 = zb->x + zb->w;
        if (ctx.cb.y1 > zb->y + zb->h) ctx.cb.y1 = zb->y + zb->h;
    }
    int y0 = t->y < ctx.cb.y0 ? ctx.cb.y0 : t->y;
    int y1 = l->y > ctx.cb.y1 ? ctx.cb.y1 : l->y;

    for (int y = y0; y < y1; y++) {
        // WHICH EDGE PAIR: the long edge top->last always, and either
        // top->mid above the middle vertex or mid->last below it.
        int longt = l->y - t->y;
        int fl = ((y - t->y) << FRAC) / longt;
        int xa = t->x + (int)(((int64_t)(l->x - t->x) * fl) >> FRAC);
        struct interp ea = lerp(at[0], at[2], fl);

        int xb;
        struct interp eb;
        if (y < m->y) {
            int d = m->y - t->y;
            int f = d ? ((y - t->y) << FRAC) / d : 0;
            xb = t->x + (int)(((int64_t)(m->x - t->x) * f) >> FRAC);
            eb = lerp(at[0], at[1], f);
        } else {
            int d = l->y - m->y;
            int f = d ? ((y - m->y) << FRAC) / d : 0;
            xb = m->x + (int)(((int64_t)(l->x - m->x) * f) >> FRAC);
            eb = lerp(at[1], at[2], f);
        }

        if (xa <= xb) span(&ctx, y, xa, xb, ea, eb);
        else          span(&ctx, y, xb, xa, eb, ea);
    }
}

void ugfx_textured_tri(struct ugfx_surface *s, const struct ugfx_texture *tex,
                       const struct ugfx_texvert *a, const struct ugfx_texvert *b,
                       const struct ugfx_texvert *c, int shade) {
    if (!tex) return;
    struct ugfx_texvert v[3] = { *a, *b, *c };
    for (int i = 0; i < 3; i++) v[i].shade = shade;
    ugfx_tri3d(s, 0, tex, 0, &v[0], &v[1], &v[2]);
}

void ugfx_textured_quad(struct ugfx_surface *s, const struct ugfx_texture *tex,
                        const struct ugfx_texvert q[4], int shade) {
    // The a-c diagonal, which keeps both triangles wound the same way as
    // the quad -- the other diagonal flips one of them and a caller
    // doing its own backface test would then disagree with itself.
    ugfx_textured_tri(s, tex, &q[0], &q[1], &q[2], shade);
    ugfx_textured_tri(s, tex, &q[0], &q[2], &q[3], shade);
}

void ugfx_texture_checker(uint32_t *px, int w, int h, int cell,
                          uint32_t a, uint32_t b) {
    if (!px || w <= 0 || h <= 0 || cell <= 0) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)y * w + x] = ((x / cell + y / cell) & 1) ? b : a;
}
