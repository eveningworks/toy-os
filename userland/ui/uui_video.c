// The picture of a playing video (ui/uui_video.h).
#include "ui/uui_video.h"
#include "ui/uui_primitives.h"
#include "lib/uvid_play.h"
#include "rt/sys.h"

#include <stdlib.h>
#include <string.h>

#define DOUBLE_NS   400000000ull    // the double-click time
#define FRAME_US     16000           // a 60 Hz frame: the bilinear budget
#define TRIAL_EVERY  120             // fast mode tries bilinear again this often

void uui_video_init(struct uui_video *v) {
    memset(v, 0, sizeof *v);
    v->bg = ugfx_rgb(0, 0, 0);
}

int uui_video_take(struct uui_video *v) {
    int c = v->committed;
    v->committed = UUI_VIDEO_NONE;
    return c;
}

static void vid_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = 0;                // whatever is going: the picture scales
    *oh = 0;
}

static void vid_geometry(void *w, int x, int y, int width, int height) {
    struct uui_video *v = w;
    v->x = x; v->y = y; v->w = width; v->h = height;
}

static void vid_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_video *v = w;
    *x = v->x; *y = v->y; *ow = v->w; *oh = v->h;
}

static void vid_draw(struct ugfx_surface *s, const void *w) {
    // The placement and the speed measurement are the widget's own
    // records of its last draw, kept through a const ops slot.
    struct uui_video *v = (struct uui_video *)w;
    const struct uvid_frame *f = v->play ? uvid_play_frame(v->play) : 0;
    if (!f || f->w <= 0 || f->h <= 0 || v->w <= 0 || v->h <= 0) {
        ugfx_fill_rect(s, v->x, v->y, v->w, v->h, v->bg);
        v->pw = v->ph = 0;
        return;
    }
    int sx = 0, sy = 0, sw = f->w, sh = f->h, dw, dh;
    if (v->cover) {
        // The larger scale, and the middle of the picture at it.
        if ((int64_t)v->w * f->h > (int64_t)v->h * f->w) { sh = (int)((int64_t)f->w * v->h / v->w); sy = (f->h - sh) / 2; }
        else { sw = (int)((int64_t)f->h * v->w / v->h); sx = (f->w - sw) / 2; }
        dw = v->w; dh = v->h;
    } else if ((int64_t)v->w * f->h > (int64_t)v->h * f->w) {
        dh = v->h; dw = (int)((int64_t)f->w * v->h / f->h);
    } else {
        dw = v->w; dh = (int)((int64_t)f->h * v->w / f->w);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = v->x + (v->w - dw) / 2, dy = v->y + (v->h - dh) / 2;
    // The bars, never the picture's own area: it is about to be covered.
    if (dy > v->y) ugfx_fill_rect(s, v->x, v->y, v->w, dy - v->y, v->bg);
    if (dy + dh < v->y + v->h) ugfx_fill_rect(s, v->x, dy + dh, v->w, v->y + v->h - dy - dh, v->bg);
    if (dx > v->x) ugfx_fill_rect(s, v->x, dy, dx - v->x, dh, v->bg);
    if (dx + dw < v->x + v->w) ugfx_fill_rect(s, dx + dw, dy, v->x + v->w - dx - dw, dh, v->bg);
    v->px = dx; v->py = dy; v->pw = dw; v->ph = dh;

    uint32_t serial = uvid_play_serial(v->play);
    if (dw != v->last_w || dh != v->last_h) {      // a new size starts a new measurement
        free(v->cache);
        v->cache = malloc((size_t)dw * (size_t)dh * sizeof *v->cache);
        v->last_w = dw;
        v->last_h = dh;
        v->fast = 0;
        v->smooth_us = 0;
        v->draws = 0;
        v->cache_serial = serial - 1;           // nothing cached at this size
    }
    if (!v->cache) return;
    if (serial != v->cache_serial || sx != v->cache_sx || sy != v->cache_sy) {
        int trial = v->fast && ++v->draws >= TRIAL_EVERY;
        int q = v->fast && !trial ? UVID_FAST : UVID_SMOOTH;
        uint64_t t0 = sys_monotonic_ns();
        uvid_frame_draw(f, sx, sy, sw, sh, v->cache, dw, dw, dh, q);
        if (q == UVID_SMOOTH) {
            uint32_t us = (uint32_t)((sys_monotonic_ns() - t0) / 1000);
            v->smooth_us = v->smooth_us ? (v->smooth_us * 7 + us) / 8 : us;
            if (trial) v->draws = 0;
            // Hysteresis: slow means over a frame, fast enough again under half.
            if (!v->fast && v->smooth_us > FRAME_US) v->fast = 1;
            else if (v->fast && v->smooth_us < FRAME_US / 2) v->fast = 0;
        }
        v->cache_serial = serial;
        v->cache_sx = sx;
        v->cache_sy = sy;
    }
    ugfx_blit(s, dx, dy, dw, dh, v->cache, dw);
}

void uui_video_free(struct uui_video *v) {
    free(v->cache);
    v->cache = 0;
    v->last_w = v->last_h = 0;
}

static int vid_hit(const void *w, int cx, int cy) {
    const struct uui_video *v = w;
    return uui_hit(v->x, v->y, v->w, v->h, cx, cy);
}

static int vid_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_video *v = w;
    v->armed = uui_hit(v->x, v->y, v->w, v->h, cx, cy);
    return v->armed;
}

static int vid_release(void *w, int cx, int cy) {
    struct uui_video *v = w;
    if (!v->armed) return 0;
    v->armed = 0;
    if (!uui_hit(v->x, v->y, v->w, v->h, cx, cy)) return 1;
    uint64_t now = sys_monotonic_ns();
    if (v->last_click_ns && now - v->last_click_ns < DOUBLE_NS) {
        v->committed = UUI_VIDEO_DOUBLE;
        v->last_click_ns = 0;
    } else {
        v->committed = UUI_VIDEO_CLICK;
        v->last_click_ns = now;
    }
    return 1;
}

static void vid_describe(const void *w, const struct uui_describe *d) {
    const struct uui_video *v = w;
    if (v->pw > 0) uui_describe_rect(d, "picture", v->px, v->py, v->pw, v->ph);
}

const struct uui_widget_ops uui_video_ops = {
    .natural_size = vid_natural,
    .set_geometry = vid_geometry,
    .bounds = vid_bounds,
    .draw = vid_draw,
    .hit = vid_hit,
    .press = vid_press,
    .release = vid_release,
    .describe = vid_describe,
};
