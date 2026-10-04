// See uregion.h. No allocation and no toolkit: this file must compile
// with a host gcc for tools/uregion_hostcheck.py.
#include "uregion.h"

void uregion_init(struct uregion *g) {
    g->n = 0;
    g->overflow = 0;
}

void uregion_init_rect(struct uregion *g, int x, int y, int w, int h) {
    uregion_init(g);
    if (w <= 0 || h <= 0) return;
    g->r[0].x = x; g->r[0].y = y; g->r[0].w = w; g->r[0].h = h;
    g->n = 1;
}

int uregion_is_empty(const struct uregion *g) { return g->n == 0; }

void uregion_intersect_rect(struct uregion *g, int x, int y, int w, int h) {
    int k = 0;
    for (int i = 0; i < g->n; i++) {
        struct urect a = g->r[i];
        int x0 = a.x > x ? a.x : x, y0 = a.y > y ? a.y : y;
        int x1 = a.x + a.w < x + w ? a.x + a.w : x + w;
        int y1 = a.y + a.h < y + h ? a.y + a.h : y + h;
        if (x1 <= x0 || y1 <= y0) continue;
        g->r[k].x = x0; g->r[k].y = y0; g->r[k].w = x1 - x0; g->r[k].h = y1 - y0;
        k++;
    }
    g->n = k;
}

// Joins a rect to one it continues exactly -- the same columns directly
// below, or the same rows directly beside -- which keeps a region cut by
// a stack of equal windows at a handful of rects rather than a ladder.
static void coalesce(struct uregion *g) {
    for (int merged = 1; merged; ) {
        merged = 0;
        for (int i = 0; i < g->n && !merged; i++)
            for (int j = 0; j < g->n && !merged; j++) {
                if (i == j) continue;
                struct urect *a = &g->r[i], *b = &g->r[j];
                if (a->x == b->x && a->w == b->w && a->y + a->h == b->y) a->h += b->h;
                else if (a->y == b->y && a->h == b->h && a->x + a->w == b->x) a->w += b->w;
                else continue;
                g->r[j] = g->r[--g->n];
                merged = 1;
            }
    }
}

void uregion_subtract_rect(struct uregion *g, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0 || g->n == 0) return;
    struct urect out[UREGION_MAX];
    int k = 0;
    for (int i = 0; i < g->n; i++) {
        struct urect a = g->r[i];
        int ax1 = a.x + a.w, ay1 = a.y + a.h;
        if (x >= ax1 || x + w <= a.x || y >= ay1 || y + h <= a.y) {
            if (k == UREGION_MAX) goto full;
            out[k++] = a;
            continue;
        }
        // The cut's overlap with `a`, then up to four pieces of `a` around
        // it: the band above, the band below, and left and right of the
        // overlap within its rows.
        int cy0 = y > a.y ? y : a.y, cy1 = y + h < ay1 ? y + h : ay1;
        int cx0 = x > a.x ? x : a.x, cx1 = x + w < ax1 ? x + w : ax1;
        struct urect piece[4];
        int np = 0;
        if (cy0 > a.y) piece[np++] = (struct urect){ a.x, a.y, a.w, cy0 - a.y };
        if (ay1 > cy1) piece[np++] = (struct urect){ a.x, cy1, a.w, ay1 - cy1 };
        if (cx0 > a.x) piece[np++] = (struct urect){ a.x, cy0, cx0 - a.x, cy1 - cy0 };
        if (ax1 > cx1) piece[np++] = (struct urect){ cx1, cy0, ax1 - cx1, cy1 - cy0 };
        if (k + np > UREGION_MAX) goto full;
        for (int p = 0; p < np; p++) out[k++] = piece[p];
    }
    for (int i = 0; i < k; i++) g->r[i] = out[i];
    g->n = k;
    coalesce(g);
    return;
full:
    g->overflow = 1;   // left as it was: a superset of the answer
}

int uregion_bbox(const struct uregion *g, struct urect *out) {
    if (g->n == 0) {
        out->x = out->y = out->w = out->h = 0;
        return 0;
    }
    int x0 = g->r[0].x, y0 = g->r[0].y;
    int x1 = x0 + g->r[0].w, y1 = y0 + g->r[0].h;
    for (int i = 1; i < g->n; i++) {
        const struct urect *a = &g->r[i];
        if (a->x < x0) x0 = a->x;
        if (a->y < y0) y0 = a->y;
        if (a->x + a->w > x1) x1 = a->x + a->w;
        if (a->y + a->h > y1) y1 = a->y + a->h;
    }
    out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0;
    return 1;
}

long long uregion_area(const struct uregion *g) {
    long long s = 0;
    for (int i = 0; i < g->n; i++) s += (long long)g->r[i].w * g->r[i].h;
    return s;
}

int uregion_contains(const struct uregion *g, int x, int y) {
    for (int i = 0; i < g->n; i++) {
        const struct urect *a = &g->r[i];
        if (x >= a->x && x < a->x + a->w && y >= a->y && y < a->y + a->h) return 1;
    }
    return 0;
}

// Equal rows, eight bytes at a time, stopping at the first difference.
// The loads go through memcpy because a row of an odd width is only
// four-byte aligned. This is at memory bandwidth on the host: a wider,
// vectorisable compare and a variant returning the first differing
// pixel both measured SLOWER (1280x720, ~0.14 ms a present).
static int row_eq(const uint32_t *a, const uint32_t *b, int w) {
    int i = 0;
    for (; i + 2 <= w; i += 2) {
        uint64_t x, y;
        __builtin_memcpy(&x, a + i, 8);
        __builtin_memcpy(&y, b + i, 8);
        if (x != y) return 0;
    }
    return i == w || a[i] == b[i];
}

void uregion_diff(struct uregion *g, const uint32_t *a, const uint32_t *b,
                  int w, int h, int stride, int max, int gap) {
    uregion_init(g);
    if (max < 1) max = 1;
    if (max > UREGION_MAX) max = UREGION_MAX;
    if (gap < 1) gap = 1;
    int y = 0;
    while (y < h) {
        if (row_eq(a + (long)y * stride, b + (long)y * stride, w)) { y++; continue; }
        // A band: from this row until `gap` equal rows in a row. A
        // differing row's span is found from both ends, and not at all
        // once the band is the full width.
        int y0 = y, last = y, x0 = w, x1 = 0;
        for (; y < h && y - last < gap; y++) {
            const uint32_t *ra = a + (long)y * stride, *rb = b + (long)y * stride;
            if (row_eq(ra, rb, w)) continue;
            last = y;
            if (x0 == 0 && x1 == w) continue;   // already the full width
            int l = 0, r = w - 1;
            while (ra[l] == rb[l]) l++;
            while (ra[r] == rb[r]) r--;
            if (l < x0) x0 = l;
            if (r + 1 > x1) x1 = r + 1;
        }
        struct urect band = { x0, y0, x1 - x0, last + 1 - y0 };
        if (g->n < max) {
            g->r[g->n++] = band;
            continue;
        }
        // No room: fold into the last band. The rows between them join it,
        // which keeps the bands disjoint and the answer a superset.
        struct urect *p = &g->r[g->n - 1];
        int px1 = p->x + p->w;
        if (band.x < p->x) p->x = band.x;
        p->w = (px1 > band.x + band.w ? px1 : band.x + band.w) - p->x;
        p->h = band.y + band.h - p->y;
    }
}
