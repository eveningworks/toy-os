// See uui_image.h.
#include "ui/uui_image.h"
#include "ui/utheme.h"
#include <string.h>

void uui_image_init(struct uui_image *im, const struct uimg *src, enum uimg_fit fit) {
    memset(im, 0, sizeof *im);
    im->img = src;
    im->fit = fit;
    im->bg = UTHEME_WINDOW_BG;
}

void uui_image_set(struct uui_image *im, const struct uimg *src) {
    uimg_free(&im->scaled);
    im->cache_src = NULL;
    im->cache_failed = 0;
    im->img = src;
}

void uui_image_set_fit(struct uui_image *im, enum uimg_fit fit) {
    if (im->fit == fit) return;
    im->fit = fit;
    im->cache_failed = 0;
}

void uui_image_release(struct uui_image *im) {
    uimg_free(&im->scaled);
    im->cache_src = NULL;
}

void uui_image_natural_size(const struct uui_image *im, int *out_w, int *out_h) {
    int w = im->img ? im->img->w : 0;
    int h = im->img ? im->img->h : 0;
    if (im->max_w && w > im->max_w) w = im->max_w;
    if (im->max_h && h > im->max_h) h = im->max_h;
    *out_w = w;
    *out_h = h;
}

// The size the picture is drawn at, before any cropping.
static void drawn_size(const struct uui_image *im, int *dw, int *dh) {
    uimg_fit_size(im->img->w, im->img->h, im->w, im->h, im->fit, dw, dh);
}

int uui_image_drawn_rect(const struct uui_image *im, int *x, int *y, int *w, int *h) {
    if (!im->img || !im->img->px || im->w <= 0 || im->h <= 0) return 0;
    int dw, dh;
    drawn_size(im, &dw, &dh);
    // Cropped when it is larger than the box (COVER always is, NONE is
    // whenever the picture is bigger than the window).
    int vw = dw < im->w ? dw : im->w;
    int vh = dh < im->h ? dh : im->h;
    *x = im->x + (im->w - vw) / 2;
    *y = im->y + (im->h - vh) / 2;
    *w = vw;
    *h = vh;
    return 1;
}

// Brings the cached scale up to date. Returns the image to blit, which
// is the SOURCE itself when no scaling is needed -- copying an image to
// draw it unchanged would double its memory for nothing.
static const struct uimg *ensure_scaled(struct uui_image *im, int dw, int dh) {
    if (!im->img || !im->img->px) return NULL;
    if (dw == im->img->w && dh == im->img->h) return im->img;
    if (im->scaled.px && im->cache_src == im->img &&
        im->cache_w == dw && im->cache_h == dh && im->cache_fit == (int)im->fit)
        return &im->scaled;
    if (im->cache_failed) return NULL;

    uimg_free(&im->scaled);
    if (uimg_scale(im->img, dw, dh, &im->scaled) < 0) {
        // Remembered, so a window that is too big to scale into does not
        // retry the allocation on every single frame.
        im->cache_failed = 1;
        im->cache_src = NULL;
        return NULL;
    }
    im->cache_src = im->img;
    im->cache_w = dw;
    im->cache_h = dh;
    im->cache_fit = (int)im->fit;
    return &im->scaled;
}

void uui_image_draw(struct ugfx_surface *s, const struct uui_image *im) {
    // The cache is written during a draw, which is why this takes a
    // const pointer and casts it away in exactly one place: every other
    // widget's draw is genuinely const, and making this one's signature
    // differ would mean it could not be in the ops table.
    struct uui_image *m = (struct uui_image *)im;
    if (im->w <= 0 || im->h <= 0) return;

    ugfx_fill_rect(s, im->x, im->y, im->w, im->h, im->bg);
    if (!im->img || !im->img->px) return;

    int dw, dh;
    drawn_size(im, &dw, &dh);
    const struct uimg *src = ensure_scaled(m, dw, dh);
    if (!src) return;

    int vx, vy, vw, vh;
    if (!uui_image_drawn_rect(im, &vx, &vy, &vw, &vh)) return;

    // When the picture is larger than its box the blit starts partway
    // into it -- centred, so a COVER wallpaper crops evenly rather than
    // losing everything on one side.
    int ox = (src->w - vw) / 2;
    int oy = (src->h - vh) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    // COMPOSITED only when the image actually has transparency. An
    // opaque 1280x720 wallpaper blended per pixel would be nearly a
    // million multiply-adds on every repaint, to arrive at the value a
    // copy already gives.
    if (src->has_alpha)
        ugfx_blit_alpha(s, vx, vy, vw, vh, src->px + (size_t)oy * src->w + ox, src->w);
    else
        ugfx_blit(s, vx, vy, vw, vh, src->px + (size_t)oy * src->w + ox, src->w);
}

// --- the ops table ----------------------------------------------------

static void ops_natural(const void *w, int *ow, int *oh) {
    uui_image_natural_size((const struct uui_image *)w, ow, oh);
}

static void ops_geom(void *w, int x, int y, int width, int height) {
    struct uui_image *im = w;
    im->x = x;
    im->y = y;
    im->w = width;
    im->h = height;
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_image *im = w;
    *x = im->x;
    *y = im->y;
    *ow = im->w;
    *oh = im->h;
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    uui_image_draw(s, (const struct uui_image *)w);
}

// No `hit`, no `press`, no `key`: a picture is not a control, so a click
// on one passes through to whatever the app wants to do with it (Image
// Viewer cycles the fit mode from its own on_press). A widget that
// consumed presses and did nothing with them would be worse.
// widget-ops-ok: display only -- no input slots, so none of the paired
// slots check_widget_ops.py enforces apply.
// The picture's own rect, which is smaller than the box when
// letterboxed -- a test sampling the box would be sampling the bars.
static void ops_describe(const void *w, const struct uui_describe *d) {
    int x, y, wd, h;
    if (uui_image_drawn_rect((const struct uui_image *)w, &x, &y, &wd, &h))
        uui_describe_rect(d, "picture", x, y, wd, h);
}

const struct uui_widget_ops uui_image_ops = {
    .natural_size = ops_natural,
    .set_geometry = ops_geom,
    .bounds       = ops_bounds,
    .draw         = ops_draw,
    .describe     = ops_describe,
};
