// Perspective-correct textured triangles -- see ui/ugfx_tex.h for why
// this lives in ugfx rather than in geom, and what the span subdivision
// is for.
#include "ui/ugfx_tex.h"
#include <stddef.h>   // size_t: the row arithmetic below

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

// One horizontal run at `y`, between two already-interpolated ends.
// Everything here is in the OVER-Z domain: uz, vz and iz are linear in
// screen space, and u and v are recovered from them by the divides
// below. That is the whole reason this looks right and affine does not.
static void span(struct ugfx_surface *s, const struct ugfx_texture *tex,
                 struct clipbox cb, int y, int xl, int xr,
                 int64_t uzl, int64_t vzl, int64_t izl,
                 int64_t uzr, int64_t vzr, int64_t izr, int shade) {
    if (xr <= xl) return;
    if (y < cb.y0 || y >= cb.y1) return;

    int w = xr - xl;
    int64_t duz = (uzr - uzl) / w, dvz = (vzr - vzl) / w, diz = (izr - izl) / w;

    // CLIPPED BY STEPPING THE INTERPOLANTS, not by starting the loop
    // late with stale values: skipping x without advancing uz/vz/iz maps
    // the wrong texel to every pixel of a partially off-screen face, and
    // it only shows when a face touches an edge.
    if (xl < cb.x0) {
        int skip = cb.x0 - xl;
        uzl += duz * skip; vzl += dvz * skip; izl += diz * skip;
        xl = cb.x0;
    }
    if (xr > cb.x1) xr = cb.x1;
    if (xr <= xl) return;

    uint32_t *row = s->pixels + (size_t)y * s->w;
    int x = xl;
    while (x < xr) {
        int n = UGFX_TEX_SPAN;
        if (x + n > xr) n = xr - x;

        // THE EXACT DIVIDE, once per span, at both ends of it.
        int64_t iz0 = izl, iz1 = izl + diz * n;
        int u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        if (iz0 > 0) { u0 = (int)((uzl << FRAC) / iz0); v0 = (int)((vzl << FRAC) / iz0); }
        if (iz1 > 0) {
            u1 = (int)(((uzl + duz * n) << FRAC) / iz1);
            v1 = (int)(((vzl + dvz * n) << FRAC) / iz1);
        }
        // ...and a plain linear walk between them, which is the saving.
        int du = n ? (u1 - u0) / n : 0, dv = n ? (v1 - v0) / n : 0;
        int u = u0, v = v0;

        for (int i = 0; i < n; i++, x++) {
            int tu = (u >> FRAC) % tex->w, tv = (v >> FRAC) % tex->h;
            if (tu < 0) tu += tex->w;
            if (tv < 0) tv += tex->h;
            row[x] = shade_px(tex->pixels[(size_t)tv * tex->w + tu], shade);
            u += du; v += dv;
        }
        uzl += duz * n; vzl += dvz * n; izl += diz * n;
    }
    ugfx_mark_dirty_rect(s, xl, y, xr - xl, 1);
}

void ugfx_textured_tri(struct ugfx_surface *s, const struct ugfx_texture *tex,
                       const struct ugfx_texvert *a, const struct ugfx_texvert *b,
                       const struct ugfx_texvert *c, int shade) {
    if (!s || !s->pixels || !tex || !tex->pixels || tex->w <= 0 || tex->h <= 0) return;
    if (a->z <= 0 || b->z <= 0 || c->z <= 0) return;   // behind the eye: caller's clip

    // Sorted top to bottom, so the sweep below has one shape rather than
    // three cases.
    const struct ugfx_texvert *t = a, *m = b, *l = c, *tmp;
    if (t->y > m->y) { tmp = t; t = m; m = tmp; }
    if (m->y > l->y) { tmp = m; m = l; l = tmp; }
    if (t->y > m->y) { tmp = t; t = m; m = tmp; }
    if (l->y == t->y) return;   // zero height

    // The over-z interpolants at each corner. `iz` is scaled up so that
    // small depth differences survive the integer divide; the scale
    // cancels when u is recovered, so its value only sets precision.
    #define IZ_SCALE (1 << 20)
    int64_t iz[3], uz[3], vz[3];
    const struct ugfx_texvert *vtx[3] = { t, m, l };
    for (int i = 0; i < 3; i++) {
        iz[i] = (int64_t)IZ_SCALE / vtx[i]->z;
        uz[i] = (int64_t)vtx[i]->u * iz[i];
        vz[i] = (int64_t)vtx[i]->v * iz[i];
    }

    struct clipbox cb = clip_of(s);
    int y0 = t->y < cb.y0 ? cb.y0 : t->y;
    int y1 = l->y > cb.y1 ? cb.y1 : l->y;

    for (int y = y0; y < y1; y++) {
        // WHICH EDGE PAIR: the long edge top->last always, and either
        // top->mid above the middle vertex or mid->last below it.
        int longt = l->y - t->y;
        int fl = longt ? ((y - t->y) << FRAC) / longt : 0;
        int xa = t->x + (((l->x - t->x) * (int64_t)fl) >> FRAC);
        int64_t uza = uz[0] + ((uz[2] - uz[0]) * fl >> FRAC);
        int64_t vza = vz[0] + ((vz[2] - vz[0]) * fl >> FRAC);
        int64_t iza = iz[0] + ((iz[2] - iz[0]) * fl >> FRAC);

        int xb; int64_t uzb, vzb, izb;
        if (y < m->y) {
            int d = m->y - t->y;
            int f = d ? ((y - t->y) << FRAC) / d : 0;
            xb = t->x + (((m->x - t->x) * (int64_t)f) >> FRAC);
            uzb = uz[0] + ((uz[1] - uz[0]) * f >> FRAC);
            vzb = vz[0] + ((vz[1] - vz[0]) * f >> FRAC);
            izb = iz[0] + ((iz[1] - iz[0]) * f >> FRAC);
        } else {
            int d = l->y - m->y;
            int f = d ? ((y - m->y) << FRAC) / d : 0;
            xb = m->x + (((l->x - m->x) * (int64_t)f) >> FRAC);
            uzb = uz[1] + ((uz[2] - uz[1]) * f >> FRAC);
            vzb = vz[1] + ((vz[2] - vz[1]) * f >> FRAC);
            izb = iz[1] + ((iz[2] - iz[1]) * f >> FRAC);
        }

        if (xa <= xb) span(s, tex, cb, y, xa, xb, uza, vza, iza, uzb, vzb, izb, shade);
        else          span(s, tex, cb, y, xb, xa, uzb, vzb, izb, uza, vza, iza, shade);
    }
    #undef IZ_SCALE
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
