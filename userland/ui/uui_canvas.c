// canvas. Split out of uwidgets.c -- see ui/uui_canvas.h.
#include "ui/uui_canvas.h"

// ---------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------

void uui_canvas_init(struct uui_canvas *c, int x, int y, int w, int h,
                      uint32_t bg, uint32_t border) {
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->bg = bg;
    c->border = border;
}

void uui_canvas_set_geometry(struct uui_canvas *c, int x, int y, int w, int h) {
    c->x = x; c->y = y; c->w = w; c->h = h;
}

void uui_canvas_begin(struct ugfx_surface *s, const struct uui_canvas *c) {
    ugfx_fill_rect(s, c->x, c->y, c->w, c->h, c->bg);
    if (c->border) ugfx_draw_rect(s, c->x, c->y, c->w, c->h, c->border);
}

int uui_canvas_cx(const struct uui_canvas *c) { return c->w / 2; }
int uui_canvas_cy(const struct uui_canvas *c) { return c->h / 2; }

int uui_canvas_hit(const struct uui_canvas *c, int cx, int cy) {
    return uui_hit(c->x, c->y, c->w, c->h, cx, cy);
}

// Clipping happens in the PLOT callback rather than by trimming each
// shape's geometry. A canvas-clipped line, ellipse and polyline would
// otherwise each need their own intersection maths, and the curved
// cases are exactly where that gets subtly wrong. One bounds test at
// the bottom cannot disagree with itself across shapes.
struct canvas_clip {
    struct ugfx_surface *s;
    const struct uui_canvas *c;
};

static void canvas_plot(void *ctx, int x, int y, uint32_t color, uint8_t alpha) {
    struct canvas_clip *cc = (struct canvas_clip *)ctx;
    // x/y arrive already offset into surface space by the callers below.
    if (x < cc->c->x || y < cc->c->y ||
        x >= cc->c->x + cc->c->w || y >= cc->c->y + cc->c->h) return;
    ugfx_blend_pixel(cc->s, x, y, color, alpha);
}

static struct geom_target canvas_target(struct canvas_clip *cc,
                                         struct ugfx_surface *s,
                                         const struct uui_canvas *c) {
    cc->s = s;
    cc->c = c;
    struct geom_target t;
    t.plot = canvas_plot;
    t.ctx = cc;
    return t;
}

void uui_canvas_line(struct ugfx_surface *s, const struct uui_canvas *c,
                      int x0, int y0, int x1, int y1, uint32_t color, enum geom_aa aa) {
    struct canvas_clip cc;
    struct geom_target t = canvas_target(&cc, s, c);
    geom_line(&t, c->x + x0, c->y + y0, c->x + x1, c->y + y1, color, aa);
}

void uui_canvas_polyline(struct ugfx_surface *s, const struct uui_canvas *c,
                          const int *xs, const int *ys, int count, int closed,
                          uint32_t color, enum geom_aa aa) {
    if (count < 2) return;
    struct canvas_clip cc;
    struct geom_target t = canvas_target(&cc, s, c);
    // Offset per segment rather than copying the arrays: a widget has no
    // business allocating, and the caller's points stay untouched.
    for (int i = 0; i + 1 < count; i++) {
        geom_line(&t, c->x + xs[i], c->y + ys[i],
                   c->x + xs[i + 1], c->y + ys[i + 1], color, aa);
    }
    if (closed) {
        geom_line(&t, c->x + xs[count - 1], c->y + ys[count - 1],
                   c->x + xs[0], c->y + ys[0], color, aa);
    }
}

void uui_canvas_ellipse(struct ugfx_surface *s, const struct uui_canvas *c,
                         int cx, int cy, int rx, int ry, uint32_t color, enum geom_aa aa) {
    struct canvas_clip cc;
    struct geom_target t = canvas_target(&cc, s, c);
    geom_ellipse(&t, c->x + cx, c->y + cy, rx, ry, color, aa);
}

void uui_canvas_circle(struct ugfx_surface *s, const struct uui_canvas *c,
                        int cx, int cy, int r, uint32_t color, enum geom_aa aa) {
    uui_canvas_ellipse(s, c, cx, cy, r, r, color, aa);
}

void uui_canvas_fill_ellipse(struct ugfx_surface *s, const struct uui_canvas *c,
                              int cx, int cy, int rx, int ry, uint32_t color) {
    struct canvas_clip cc;
    struct geom_target t = canvas_target(&cc, s, c);
    geom_fill_ellipse(&t, c->x + cx, c->y + cy, rx, ry, color);
}

void uui_canvas_fill_polygon(struct ugfx_surface *s, const struct uui_canvas *c,
                              const int *xs, const int *ys, int count, uint32_t color) {
    if (count < 3 || count > GEOM_POLY_MAX) return;
    int sx[GEOM_POLY_MAX], sy[GEOM_POLY_MAX];
    for (int i = 0; i < count; i++) { sx[i] = c->x + xs[i]; sy[i] = c->y + ys[i]; }
    struct canvas_clip cc;
    struct geom_target t = canvas_target(&cc, s, c);
    geom_fill_polygon(&t, sx, sy, count, color);
}
