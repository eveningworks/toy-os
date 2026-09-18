// See wm_shadow.h.
#include "wm_internal.h"
#include "wm_shadow.h"
#include "wm/wm_conf.h"
#include "ui/ugfx.h"

#define DESKTOP_CONF "/etc/desktop.conf"

static int g_enabled = 1;
static uint32_t g_seen_generation;

// Radius (how far the shadow reaches), darkness (the alpha at the edge
// of the casting rect, of 255) and downward offset, per kind. In LINE
// HEIGHTS so they scale with the chrome: Breeze's active shadow is
// about a title bar tall.
static void params(enum wm_shadow_kind kind, int *r, int *a, int *oy) {
    int lh = ugfx_char_h();
    if (lh <= 0) lh = 14;
    switch (kind) {
    // Breeze's default shadow strength is 50%; its inactive shadow is
    // smaller and lighter, which is how depth says which window is up.
    case WM_SHADOW_FOCUSED:  *r = lh * 3 / 2; *a = 128; break;
    case WM_SHADOW_INACTIVE: *r = lh;         *a = 72;  break;
    case WM_SHADOW_POPUP:    *r = lh * 3 / 4; *a = 96;  break;
    default:                 *r = 0;          *a = 0;  break;
    }
    if (*r < 4 && *r > 0) *r = 4;
    *oy = *r / 3;
}

int wm_shadow_margin(void) {
    int r, a, oy;
    params(WM_SHADOW_FOCUSED, &r, &a, &oy);
    return r + oy;
}

static int isqrt(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

// The falloff past the casting edge: 1 at the edge, 0 at `r`, quadratic
// -- close to a blurred box edge and needs no blur.
static int fall_alpha(int d, int r, int a) {
    if (d <= 0) return a;
    if (d >= r) return 0;
    int u = r - d;                   // u/r in (0,1)
    return a * u * u / (r * r);
}

// A CORNER TILE: alpha at (i, j) pixels outward from the arc centre of
// a corner whose window corner is rounded by `cr`, cached per kind and
// recomputed when the font (and so the radii) changes. Four corners
// use one tile mirrored, which is what makes a shadow cost a
// perimeter's worth of blends and no square roots per frame.
struct tile { int r, cr, a, side; unsigned char *px; };
static struct tile g_tile[4];
static unsigned char g_tile_px[4][64 * 64];

static const struct tile *corner_tile(enum wm_shadow_kind kind, int cr) {
    int r, a, oy;
    params(kind, &r, &a, &oy);
    struct tile *t = &g_tile[kind];
    int side = r + cr;
    if (side > 64) side = 64;
    if (t->px && t->r == r && t->cr == cr && t->a == a && t->side == side) return t;
    t->r = r; t->cr = cr; t->a = a; t->side = side; t->px = g_tile_px[kind];
    for (int j = 0; j < side; j++)
        for (int i = 0; i < side; i++) {
            // (i, j) is the pixel (i + 1, j + 1) past an arc centre:
            // its distance to the arc, signed, is the corner's falloff.
            int d = isqrt((i + 1) * (i + 1) + (j + 1) * (j + 1)) - cr;
            t->px[j * side + i] = (unsigned char)fall_alpha(d, r, a);
        }
    return t;
}

// The alpha of the shadow at (px, py) for a casting rect with arc
// centres (cx0..cx1, cy0..cy1): the corner tile in the diagonal
// regions, the 1-D falloff along an edge, full inside.
static int shadow_alpha(const struct tile *t, int px, int py,
                        int cx0, int cx1, int cy0, int cy1) {
    int dx = px < cx0 ? cx0 - px : (px > cx1 ? px - cx1 : 0);
    int dy = py < cy0 ? cy0 - py : (py > cy1 ? py - cy1 : 0);
    if (dx && dy) {
        if (dx > t->side || dy > t->side) return 0;
        return t->px[(dy - 1) * t->side + (dx - 1)];
    }
    int d = (dx > dy ? dx : dy) - t->cr;
    return fall_alpha(d, t->r, t->a);
}

void wm_shadow_draw(int x, int y, int w, int h, int corner_r, enum wm_shadow_kind kind) {
    if (!g_enabled || kind == WM_SHADOW_NONE || w <= 0 || h <= 0) return;
    int r, a, oy;
    params(kind, &r, &a, &oy);
    if (r <= 0) return;
    struct ugfx_surface *s = wm_surface();
    int cr = corner_r > 0 ? corner_r : 0;
    const struct tile *t = corner_tile(kind, cr);

    // The casting rect sits `oy` below the window; its arc centres.
    int sx = x, sy = y + oy;
    int cx0 = sx + cr, cx1 = sx + w - 1 - cr;
    int cy0 = sy + cr, cy1 = sy + h - 1 - cr;

    // Everything the shadow can reach, less the surface and the clip.
    // A POPUP'S SHADOW FALLS DOWNWARD ONLY: a menu hangs from the bar
    // or the control that opened it, and a shadow reaching up over
    // that bar reads as the bar changing when the menu opens. Windows
    // menus and Breeze's both keep the shadow below the top edge.
    int bx0 = sx - r, bx1 = sx + w + r, by0 = sy - r, by1 = sy + h + r;
    if (kind == WM_SHADOW_POPUP && by0 < y) by0 = y;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > s->w) bx1 = s->w;
    if (by1 > s->h) by1 = s->h;
    if (s->clip_active) {
        if (bx0 < s->clip_x0) bx0 = s->clip_x0;
        if (by0 < s->clip_y0) by0 = s->clip_y0;
        if (bx1 > s->clip_x1) bx1 = s->clip_x1;
        if (by1 > s->clip_y1) by1 = s->clip_y1;
    }
    // What the window will paint over anyway, less its corner boxes:
    // skipped, since the rounded cut is the only place a covered pixel
    // shows through.
    int ix0 = x + cr, ix1 = x + w - cr, iy0 = y + cr, iy1 = y + h - cr;

    for (int py = by0; py < by1; py++) {
        int inside_rows = (py >= iy0 && py < iy1);
        for (int px = bx0; px < bx1; px++) {
            if (inside_rows && px >= ix0 && px < ix1) { px = ix1 - 1; continue; }
            int al = shadow_alpha(t, px, py, cx0, cx1, cy0, cy1);
            if (al > 0) ugfx_blend_pixel(s, px, py, 0x000000, (uint8_t)al);
        }
    }
}

void wm_damage_window_rect(int x, int y, int w, int h) {
    int m = g_enabled ? wm_shadow_margin() : 0;
    m -= wm_damage_shrink_px();   // the sweep's positive control (wm_internal.h)
    wm_damage_rect(x - m, y - m, w + 2 * m, h + 2 * m);
}

int wm_shadow_enabled(void) { return g_enabled; }

static void adopt(void) {
    char v[16];
    int on = 1;
    if (wm_conf_get(DESKTOP_CONF, "shadows", v, sizeof v) && v[0])
        on = !(v[0] == 'o' && v[1] == 'f' && v[2] == 'f');
    if (on != g_enabled) {
        g_enabled = on;
        redraw_pending = 1;   // a full frame: nothing reported damage for the shadows
    }
}

void wm_shadow_poll_config(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return;
    g_seen_generation = gen;
    adopt();
}
