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

// WHERE THE WHOLE PICTURE SITS, content coordinates, possibly larger than
// the view: (ox, oy) its top-left, dw x dh its size on screen. A fit mode
// centres it (a COVER wallpaper crops evenly); a zoom centres an axis that
// fits and scrolls one that does not.
static void placement(const struct uui_image *im, int *ox, int *oy, int *dw, int *dh) {
    if (im->zoom > 0) {
        *dw = (int)((long)im->img->w * im->zoom / 100); if (*dw < 1) *dw = 1;
        *dh = (int)((long)im->img->h * im->zoom / 100); if (*dh < 1) *dh = 1;
        *ox = *dw <= im->w ? im->x + (im->w - *dw) / 2 : im->x - im->pan_x;
        *oy = *dh <= im->h ? im->y + (im->h - *dh) / 2 : im->y - im->pan_y;
        return;
    }
    drawn_size(im, dw, dh);
    *ox = im->x + (im->w - *dw) / 2;
    *oy = im->y + (im->h - *dh) / 2;
}

static void clamp_pan(struct uui_image *im) {
    if (!im->img || im->zoom <= 0) { im->pan_x = im->pan_y = 0; return; }
    int dw = (int)((long)im->img->w * im->zoom / 100);
    int dh = (int)((long)im->img->h * im->zoom / 100);
    int mx = dw - im->w, my = dh - im->h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (im->pan_x > mx) im->pan_x = mx;
    if (im->pan_y > my) im->pan_y = my;
    if (im->pan_x < 0) im->pan_x = 0;
    if (im->pan_y < 0) im->pan_y = 0;
}

void uui_image_set_zoom(struct uui_image *im, int pct, int ax, int ay) {
    if (!im->img || !im->img->w || !im->img->h) return;
    if (pct <= 0) { im->zoom = 0; im->pan_x = im->pan_y = 0; return; }
    if (pct < 5) pct = 5;
    if (pct > 1600) pct = 1600;
    if (ax < 0 || ay < 0) { ax = im->x + im->w / 2; ay = im->y + im->h / 2; }

    // The picture point under the anchor, as a fraction of the picture
    // (16.16), before and after -- so it stays under the pointer.
    int ox, oy, dw, dh;
    placement(im, &ox, &oy, &dw, &dh);
    long fx = ((long)(ax - ox) << 16) / dw, fy = ((long)(ay - oy) << 16) / dh;
    if (fx < 0) fx = 0;
    if (fx > 65536) fx = 65536;
    if (fy < 0) fy = 0;
    if (fy > 65536) fy = 65536;

    im->zoom = pct;
    int nw = (int)((long)im->img->w * pct / 100), nh = (int)((long)im->img->h * pct / 100);
    im->pan_x = (int)((fx * nw) >> 16) - (ax - im->x);
    im->pan_y = (int)((fy * nh) >> 16) - (ay - im->y);
    clamp_pan(im);
}

int uui_image_zoom_pct(const struct uui_image *im) {
    if (!im->img || !im->img->w) return 0;
    if (im->zoom > 0) return im->zoom;
    int dw, dh;
    drawn_size(im, &dw, &dh);
    return (int)((long)dw * 100 / im->img->w);
}

int uui_image_pan(struct uui_image *im, int dx, int dy) {
    int px = im->pan_x, py = im->pan_y;
    im->pan_x -= dx;
    im->pan_y -= dy;
    clamp_pan(im);
    return px != im->pan_x || py != im->pan_y;
}

int uui_image_can_pan(const struct uui_image *im) {
    if (!im->img || im->zoom <= 0) return 0;
    return (long)im->img->w * im->zoom / 100 > im->w ||
           (long)im->img->h * im->zoom / 100 > im->h;
}

int uui_image_drawn_rect(const struct uui_image *im, int *x, int *y, int *w, int *h) {
    if (!im->img || !im->img->px || im->w <= 0 || im->h <= 0) return 0;
    // The part of the picture inside the view: cropped when it is larger
    // (COVER always is, NONE and a zoom whenever the picture outgrows it).
    int ox, oy, dw, dh;
    placement(im, &ox, &oy, &dw, &dh);
    int x0 = ox > im->x ? ox : im->x, y0 = oy > im->y ? oy : im->y;
    int x1 = ox + dw < im->x + im->w ? ox + dw : im->x + im->w;
    int y1 = oy + dh < im->y + im->h ? oy + dh : im->y + im->h;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

// Brings the cached scale up to date. Returns the image to blit, which
// is the SOURCE itself when no scaling is needed -- copying an image to
// draw it unchanged would double its memory for nothing.
static const struct uimg *ensure_scaled(struct uui_image *im, int dw, int dh) {
    if (!im->img || !im->img->px) return NULL;
    if (dw == im->img->w && dh == im->img->h) return im->img;
    if (im->scaled.px && im->cache_src == im->img &&
        im->cache_w == dw && im->cache_h == dh)
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

    if (!im->transparent) ugfx_fill_rect(s, im->x, im->y, im->w, im->h, im->bg);
    if (!im->img || !im->img->px) return;

    int px, py, dw, dh;
    placement(im, &px, &py, &dw, &dh);
    int vx, vy, vw, vh;
    if (!uui_image_drawn_rect(im, &vx, &vy, &vw, &vh)) return;

    // MAGNIFIED: only the visible part of the SOURCE, nearest neighbour --
    // pixels a viewer at 400% is expected to show as squares. The source
    // rect is rounded outward and the blit clipped to the view, so the
    // edge cells are whole rather than shifted. That blit ignores the
    // source's alpha, so a transparent PNG shows its colour channels here.
    if (im->zoom >= 100) {
        int z = im->zoom;
        int sx0 = (int)((long)(vx - px) * 100 / z), sy0 = (int)((long)(vy - py) * 100 / z);
        int sx1 = (int)(((long)(vx + vw - px) * 100 + z - 1) / z);
        int sy1 = (int)(((long)(vy + vh - py) * 100 + z - 1) / z);
        if (sx1 > im->img->w) sx1 = im->img->w;
        if (sy1 > im->img->h) sy1 = im->img->h;
        if (sx1 <= sx0 || sy1 <= sy0) return;
        struct ugfx_clip c;
        ugfx_clip_save(s, &c);
        ugfx_clip_intersect(s, vx, vy, vw, vh);
        ugfx_blit_scaled_alpha(s, px + (int)((long)sx0 * z / 100), py + (int)((long)sy0 * z / 100),
                               (int)((long)(sx1 - sx0) * z / 100), (int)((long)(sy1 - sy0) * z / 100),
                               im->img->px + (size_t)sy0 * im->img->w + sx0,
                               sx1 - sx0, sy1 - sy0, im->img->w, 255);
        ugfx_clip_restore(s, &c);
        return;
    }

    const struct uimg *src = ensure_scaled(m, dw, dh);
    if (!src) return;
    // Where the view starts inside the (scaled) picture: the centred crop
    // of a COVER wallpaper, or the pan of a zoom.
    int ox = vx - px;
    int oy = vy - py;
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
