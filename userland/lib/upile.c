// Falling debris and the pile it makes -- see lib/upile.h.
#include "lib/upile.h"

#include <stdlib.h>
#include <string.h>

#include "fixed.h"
#include "rt/sys.h"

static uint32_t rnd(struct upile *p) {
    uint32_t x = p->rng ? p->rng : 0x2545f491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return p->rng = x;
}

static void dirty(struct upile *p, int x0, int y0, int x1, int y1) {
    if (p->dx1 <= p->dx0) { p->dx0 = x0; p->dy0 = y0; p->dx1 = x1; p->dy1 = y1; return; }
    if (x0 < p->dx0) p->dx0 = x0;
    if (y0 < p->dy0) p->dy0 = y0;
    if (x1 > p->dx1) p->dx1 = x1;
    if (y1 > p->dy1) p->dy1 = y1;
}

int upile_init(struct upile *p, uint32_t *bake, int w, int h, int cell) {
    memset(p, 0, sizeof *p);
    if (cell < 1) cell = 1;
    p->w = w;
    p->h = h;
    p->cell = cell;
    p->cols = (w + cell - 1) / cell;
    p->bake = bake;
    p->top = calloc((size_t)p->cols, sizeof *p->top);
    p->clean = malloc((size_t)w * (size_t)h * 4);
    if (!p->top || !p->clean) { upile_free(p); return 0; }
    memcpy(p->clean, bake, (size_t)w * (size_t)h * 4);
    sys_getrandom(&p->rng, sizeof p->rng);
    return 1;
}

void upile_free(struct upile *p) {
    free(p->top);
    free(p->clean);
    p->top = 0;
    p->clean = 0;
}

void upile_add(struct upile *p, int kind, int x, int y, int vx, int vy, int size, uint32_t color) {
    if (p->n >= UPILE_PARTS || !p->top) return;
    struct upile_part *q = &p->p[p->n++];
    q->x = x << 8;
    q->y = y << 8;
    q->vx = vx << 8;
    q->vy = vy << 8;
    q->ang = (uint16_t)rnd(p);
    q->va = (int16_t)((int)(rnd(p) % 8000) - 4000);
    q->kind = (uint8_t)kind;
    q->size = (uint8_t)(size < 1 ? 1 : size > 40 ? 40 : size);
    q->color = color;
}

static int col_of(const struct upile *p, int x) {
    int c = x / p->cell;
    return c < 0 ? 0 : c >= p->cols ? p->cols - 1 : c;
}

static void paint(struct upile *p, int x0, int y0, int w, int h, uint32_t color) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= p->h) continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < p->w) p->bake[(size_t)y * p->w + x] = color;
    }
    dirty(p, x0, y0, x0 + w, y0 + h);
}

// A grain joins the pile: it rolls to a lower neighbour while the step
// down is more than one grain -- the sandpile rule -- then is painted in.
static void deposit(struct upile *p, int x, uint32_t color) {
    int c = col_of(p, x), cell = p->cell;
    for (int k = 0; k < 64; k++) {
        int l = c > 0 ? p->top[c - 1] : 0xffff, r = c + 1 < p->cols ? p->top[c + 1] : 0xffff;
        int lo = l < r ? l : r;
        if (p->top[c] - lo <= cell) break;
        if (l < r || (l == r && (rnd(p) & 1))) c--; else c++;
    }
    if (p->top[c] + cell >= p->h) return;
    paint(p, c * cell, p->h - p->top[c] - cell, cell, cell, color);
    p->top[c] = (uint16_t)(p->top[c] + cell);
}

static uint32_t darker(uint32_t c, int pct) {
    return ((((c >> 16) & 255) * pct / 100) << 16) | ((((c >> 8) & 255) * pct / 100) << 8) |
           ((c & 255) * pct / 100);
}

// A shard's three corners: uneven, so the pieces do not all look alike.
static void shard_pts(const struct upile_part *q, int xs[3], int ys[3]) {
    static const int OFF[3] = { 0, 23000, 42000 };    // turns / 65536
    static const int RAD[3] = { 100, 75, 90 };        // % of size
    int cx = q->x >> 8, cy = q->y >> 8;
    for (int k = 0; k < 3; k++) {
        fx_t a = (fx_t)(((uint32_t)q->ang + (uint32_t)OFF[k]) & 0xffff);   // 16.16 turns
        int r = q->size * RAD[k] / 100;
        xs[k] = cx + (int)((fx_cos(a) * r) >> FX_SHIFT);
        ys[k] = cy + (int)((fx_sin(a) * r) >> FX_SHIFT);
    }
}

// A shard at rest is painted into the pile and lifts it under its span.
static void settle(struct upile *p, const struct upile_part *q) {
    int xs[3], ys[3];
    shard_pts(q, xs, ys);
    struct ugfx_surface s = ugfx_surface_for_pixels(p->bake, p->w, p->h);
    ugfx_fill_polygon(&s, xs, ys, 3, darker(q->color, 85));
    int x0 = xs[0], x1 = xs[0], y0 = ys[0];
    for (int k = 1; k < 3; k++) {
        if (xs[k] < x0) x0 = xs[k];
        if (xs[k] > x1) x1 = xs[k];
        if (ys[k] < y0) y0 = ys[k];
    }
    dirty(p, x0 - 1, y0 - 1, x1 + 2, p->h);
    int lift = p->h - (y0 + (q->y >> 8)) / 2;   // half its height above the ground it sits on
    for (int c = col_of(p, x0); c <= col_of(p, x1); c++)
        if (lift > p->top[c] && lift < p->h) p->top[c] = (uint16_t)lift;
}

// One pixel down, every column: what was under the pile's top row comes
// back from the clean copy.
static void sink(struct upile *p) {
    int changed = 0;
    for (int c = 0; c < p->cols; c++) {
        if (!p->top[c]) continue;
        int x0 = c * p->cell, x1 = x0 + p->cell > p->w ? p->w : x0 + p->cell;
        int ytop = p->h - p->top[c];
        for (int y = p->h - 1; y > ytop; y--)
            memcpy(p->bake + (size_t)y * p->w + x0, p->bake + (size_t)(y - 1) * p->w + x0,
                   (size_t)(x1 - x0) * 4);
        memcpy(p->bake + (size_t)ytop * p->w + x0, p->clean + (size_t)ytop * p->w + x0,
               (size_t)(x1 - x0) * 4);
        p->top[c]--;
        dirty(p, x0, ytop, x1, p->h);
        changed = 1;
    }
    (void)changed;
}

int upile_floor(const struct upile *p, int x0, int x1) {
    if (!p->top) return p->h;
    int m = 0;
    for (int c = col_of(p, x0); c <= col_of(p, x1); c++)
        if (p->top[c] > m) m = p->top[c];
    return p->h - m;
}

void upile_step(struct upile *p, int dt_ms) {
    if (!p->top || dt_ms <= 0) return;
    int g = p->h * 5 / 3;   // px/s^2: a fall across the screen takes about a second
    for (int i = 0; i < p->n;) {
        struct upile_part *q = &p->p[i];
        q->vy += (int32_t)((int64_t)g * 256 * dt_ms / 1000);
        q->x += (int32_t)((int64_t)q->vx * dt_ms / 1000);
        q->y += (int32_t)((int64_t)q->vy * dt_ms / 1000);
        q->ang = (uint16_t)(q->ang + q->va * 4 * dt_ms / 1000);
        int x = q->x >> 8;
        if (x < 1)        { q->x = 1 << 8;          q->vx = -q->vx * 2 / 5; }
        if (x > p->w - 2) { q->x = (p->w - 2) << 8; q->vx = -q->vx * 2 / 5; }
        int half = q->kind == UPILE_SHARD ? q->size / 3 : 0;
        int ground = p->h - p->top[col_of(p, q->x >> 8)];
        int gone = 0;
        if ((q->y >> 8) + half >= ground) {
            if (q->kind == UPILE_GRAIN) {
                deposit(p, q->x >> 8, q->color);
                gone = 1;
            } else {
                q->y = (ground - half) << 8;
                q->vy = -q->vy * 35 / 100;
                q->vx = q->vx * 6 / 10;
                q->va /= 2;
                // AT REST: a bounce too small to see. A crumbling shard
                // throws a few grains; a whole one is painted in.
                if (q->vy > -(g * 256 / 14)) {
                    if (p->crumble) {
                        struct upile_part was = *q;
                        p->p[i] = p->p[--p->n];
                        // AS MUCH SAND AS THE SHARD HAD AREA, about: a
                        // pile that came out smaller than what fell in
                        // would read as debris vanishing.
                        int bits = was.size * was.size / (2 * p->cell * p->cell);
                        if (bits < 3) bits = 3;
                        if (bits > 48) bits = 48;
                        for (int k = 0; k < bits; k++)
                            upile_add(p, UPILE_GRAIN, (was.x >> 8) + (int)(rnd(p) % 9) - 4,
                                      (was.y >> 8) - 2, (int)(rnd(p) % 80) - 40,
                                      -30 - (int)(rnd(p) % 60), p->cell,
                                      darker(was.color, 80 + (int)(rnd(p) % 30)));
                        continue;
                    }
                    settle(p, q);
                    gone = 1;
                }
            }
        }
        if (gone) p->p[i] = p->p[--p->n];
        else i++;
    }
    if (p->sink_at > 0) {
        int m = 0;
        for (int c = 0; c < p->cols; c++) if (p->top[c] > m) m = p->top[c];
        p->sink_ms += dt_ms;
        if (m <= p->sink_at) p->sink_ms = 0;
        while (p->sink_ms >= 120) { p->sink_ms -= 120; sink(p); }
    }
}

void upile_draw(struct upile *p, struct ugfx_surface *s, int *x0, int *y0, int *x1, int *y1) {
    *x0 = *y0 = 1 << 30;
    *x1 = *y1 = -(1 << 30);
    for (int i = 0; i < p->n; i++) {
        const struct upile_part *q = &p->p[i];
        int cx = q->x >> 8, cy = q->y >> 8, r = q->size + 2;
        if (q->kind == UPILE_GRAIN) ugfx_fill_rect(s, cx - q->size / 2, cy - q->size / 2, q->size, q->size, q->color);
        else {
            int xs[3], ys[3];
            shard_pts(q, xs, ys);
            ugfx_fill_polygon(s, xs, ys, 3, q->color);
        }
        if (cx - r < *x0) *x0 = cx - r;
        if (cy - r < *y0) *y0 = cy - r;
        if (cx + r > *x1) *x1 = cx + r;
        if (cy + r > *y1) *y1 = cy + r;
    }
}

int upile_take_dirty(struct upile *p, int *x0, int *y0, int *x1, int *y1) {
    if (p->dx1 <= p->dx0) return 0;
    *x0 = p->dx0; *y0 = p->dy0; *x1 = p->dx1; *y1 = p->dy1;
    p->dx0 = p->dy0 = p->dx1 = p->dy1 = 0;
    return 1;
}
