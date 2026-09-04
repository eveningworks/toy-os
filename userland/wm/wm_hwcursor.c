// The hardware cursor plane, consumed at last (wm_render.c's own
// comment promised it back "with virtio-input's absolute pointer in
// Milestone 27a" -- this is that return). The compositor DEFINES a
// sprite when the shape changes and otherwise draws nothing: the
// kernel moves the plane on every pointer event (win_input.c), and on
// a host whose display renders the plane as the real host pointer
// (QEMU + virtio-gpu) motion is host-latency and the window edge has
// nothing to cross.
//
// THE SPRITE IS BUILT FROM THE SAME MASKS THE SOFTWARE PATH DRAWS --
// cursor_theme.h has said since it was written that the mask pair
// exists so a hardware plane can consume it. Colours are composited
// here once per shape change (outline under fill, over transparency)
// instead of per pixel per frame.
//
// WHEN THE PLANE CANNOT SHOW A SHAPE, THE SOFTWARE SPRITE DOES: no
// plane on this driver, a themed shape scaled past the plane's 64x64
// (cursor_size=huge is 3x), or a built-in shape that exists only as
// draw calls (everything but the arrow). wm_hwcursor_sync() answers
// per shape, so the two paths hand over cleanly instead of both or
// neither drawing -- the stranded-sprite class the roadmap warns
// about.
#include "wm_internal.h"
#include "kapi.h"
#include "cursor_theme.h"
#include "win_proto.h"
#include "rt/sys.h"
#include "ui/utheme.h"
#include "wm_log.h"

#define HWC_DIM 64

static int g_avail = -1;   // -1 unqueried; the answer never changes
static int g_shown = 0;
static int g_kind = -1;    // the kind the plane currently shows
static int g_stale = 1;    // theme/scale changed: rebuild whatever g_kind says

static uint32_t g_sprite[HWC_DIM * HWC_DIM];

static int fb_cursor_req(int op, int32_t b, int32_t c, int32_t d) {
    struct win_request_msg q;
    k_memset(&q, 0, sizeof q);
    q.type = WIN_REQ_FB_CURSOR;
    q.a = op;
    q.b = b;
    q.c = c;
    q.d = d;
    return sys_win_request(&q);
}

int wm_hwcursor_available(void) {
    if (g_avail < 0)
        g_avail = fb_cursor_req(WIN_FB_CURSOR_QUERY, 0, 0, 0) == 1 ? 1 : 0;
    return g_avail;
}

void wm_hwcursor_invalidate(void) { g_stale = 1; }

int wm_hwcursor_active(void) { return g_shown; }

static void hwc_hide(void) {
    if (!g_shown) return;
    fb_cursor_req(WIN_FB_CURSOR_HIDE, 0, 0, 0);
    g_shown = 0;
    g_kind = -1;
}

// Outline (black) under fill (the theme's white), over transparency --
// the same two passes draw_cursor_normal() makes per frame, folded
// into straight ARGB once. `sc` is nearest-neighbour, matching the
// software path's scaled draw.
static void build_sprite(const unsigned char *outline, const unsigned char *fill,
                          int w, int h, int stride, int sc, int *out_w, int *out_h) {
    uint32_t fill_rgb = UTHEME_WHITE;
    int dw = w * sc, dh = h * sc;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int o = outline[(y / sc) * stride + (x / sc)];
            int f = fill[(y / sc) * stride + (x / sc)];
            int a = f + o * (255 - f) / 255;
            uint32_t px = 0;
            if (a > 0) {
                // Only the fill carries colour; the outline is black,
                // so each channel is the fill's share of the coverage.
                uint32_t r = ((fill_rgb >> 16) & 0xFF) * (uint32_t)f / (uint32_t)a;
                uint32_t g = ((fill_rgb >> 8) & 0xFF) * (uint32_t)f / (uint32_t)a;
                uint32_t bl = (fill_rgb & 0xFF) * (uint32_t)f / (uint32_t)a;
                px = ((uint32_t)a << 24) | (r << 16) | (g << 8) | bl;
            }
            g_sprite[y * dw + x] = px;
        }
    }
    *out_w = dw;
    *out_h = dh;
}

int wm_hwcursor_sync(enum wm_cursor_kind kind) {
    if (!wm_hwcursor_available()) return 0;

    const struct cursor_shape *s = cursor_theme_shape(kind);
    int sc = cursor_theme_scale();

    const unsigned char *outline, *fill;
    int w, h, stride, hot_x, hot_y;
    if (s) {
        outline = &s->outline[0][0];
        fill = &s->fill[0][0];
        w = s->w; h = s->h; stride = CURSOR_SHAPE_MAX;
        hot_x = s->hot_x; hot_y = s->hot_y;
    } else if (kind == WM_CURSOR_NORMAL) {
        wm_builtin_arrow_masks(&outline, &fill, &w, &h, &stride);
        hot_x = 0; hot_y = 0;
        sc = 1; // the built-in arrow was never scaled by the setting
    } else {
        // A built-in resize/text/wait shape is draw calls, not masks.
        hwc_hide();
        return 0;
    }
    if (w * sc > HWC_DIM || h * sc > HWC_DIM) {
        hwc_hide(); // cursor_size=huge: true size beats plane smoothness
        return 0;
    }

    if ((int)kind != g_kind || g_stale) {
        int dw, dh;
        build_sprite(outline, fill, w, h, stride, sc, &dw, &dh);
        uint64_t up = (uint64_t)(uintptr_t)g_sprite;
        int32_t d = (int32_t)(((uint32_t)dw << 24) | ((uint32_t)dh << 16) |
                               ((uint32_t)(hot_x * sc) << 8) | (uint32_t)(hot_y * sc));
        if (fb_cursor_req(WIN_FB_CURSOR_DEFINE, (int32_t)(up & 0xFFFFFFFFu),
                           (int32_t)(up >> 32), d) != 0) {
            // The plane refused (mid-flight driver trouble): fall back
            // rather than showing yesterday's shape.
            hwc_hide();
            g_avail = 0;
            return 0;
        }
        g_kind = (int)kind;
        g_stale = 0;
    }
    if (!g_shown) {
        if (fb_cursor_req(WIN_FB_CURSOR_SHOW, 0, 0, 0) != 0) {
            g_avail = 0;
            return 0;
        }
        g_shown = 1;
    }
    return 1;
}
