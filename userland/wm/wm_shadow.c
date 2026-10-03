// See wm_shadow.h.
#include "wm_internal.h"
#include "wm_shadow.h"
#include "wm/wm_conf.h"
#include "ui/ugfx.h"
#include <stdlib.h>
#include <string.h>

#define DESKTOP_CONF "/etc/desktop.conf"

static int g_enabled = 1;
static uint32_t g_seen_generation;

// The edge falloff's domain (d past the casting edge) and the widest
// varying run a row can carry, both bounds on font-derived radii.
#define SHADOW_EDGE_MAX 256
#define SHADOW_SPAN_MAX 320

// Radius (how far the shadow reaches), darkness (the alpha at the edge
// of the casting rect, of 255) and downward offset, per kind. In LINE
// HEIGHTS so they scale with the chrome: Plasma's active shadow is
// about a title bar tall.
static void params(enum wm_shadow_kind kind, int *r, int *a, int *oy) {
    int lh = ugfx_char_h();
    if (lh <= 0) lh = 14;
    switch (kind) {
    // Plasma's default shadow strength is 50%; its inactive shadow is
    // smaller and lighter, which is how depth says which window is up.
    case WM_SHADOW_FOCUSED:  *r = lh * 3 / 2; *a = 128; break;
    case WM_SHADOW_INACTIVE: *r = lh;         *a = 72;  break;
    case WM_SHADOW_POPUP:    *r = lh * 3 / 4; *a = 96;  break;
    default:                 *r = 0;          *a = 0;  break;
    }
    if (*r < 4 && *r > 0) *r = 4;
    if (*r >= SHADOW_EDGE_MAX) *r = SHADOW_EDGE_MAX - 1;  // the edge LUT's domain
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
struct tile { int r, cr, a, side; unsigned char *px; unsigned char edge[SHADOW_EDGE_MAX]; };
static struct tile g_tile[4];
static unsigned char g_tile_px[4][64 * 64];

// The straight edges' falloff, cached beside the corner tile. It was
// fall_alpha() per pixel, and that division ran once for every pixel of
// a window's perimeter band, every frame of a drag.
static int edge_alpha(const struct tile *t, int d) {
    if (d <= 0) return t->a;
    if (d >= t->r) return 0;
    return t->edge[d];
}

static const struct tile *corner_tile(enum wm_shadow_kind kind, int cr) {
    int r, a, oy;
    params(kind, &r, &a, &oy);
    struct tile *t = &g_tile[kind];
    int side = r + cr;
    if (side > 64) side = 64;
    if (t->px && t->r == r && t->cr == cr && t->a == a && t->side == side) return t;
    t->r = r; t->cr = cr; t->a = a; t->side = side; t->px = g_tile_px[kind];
    for (int d = 0; d < SHADOW_EDGE_MAX; d++)
        t->edge[d] = (unsigned char)fall_alpha(d, r, a);
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
    return edge_alpha(t, d);
}

// THE SHADOW ALREADY ON EACH PIXEL since an opaque paint last covered
// it, one byte per screen pixel (wm_shadow.h). NULL when it could not be
// had, which degrades to the old compounding rather than to no shadow.
static unsigned char *g_rec;
static int g_rec_w, g_rec_h;

// The screen's back buffer only: a ghost snapshot (wm_anim.c) draws into
// a buffer of its own size, and would otherwise reallocate this.
static int rec_ready(const struct ugfx_surface *s) {
    if (s != &g_wm_screen.back) return 0;
    if (g_rec && g_rec_w == s->w && g_rec_h == s->h) return 1;
    free(g_rec);
    g_rec = calloc((size_t)s->w * (size_t)s->h, 1);
    g_rec_w = g_rec ? s->w : 0;
    g_rec_h = g_rec ? s->h : 0;
    return g_rec != 0;
}

void wm_shadow_cover(int x, int y, int w, int h) {
    if (!g_enabled) return;
    struct ugfx_surface *s = wm_surface();
    if (!rec_ready(s)) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->clip_active) {
        if (x0 < s->clip_x0) x0 = s->clip_x0;
        if (y0 < s->clip_y0) y0 = s->clip_y0;
        if (x1 > s->clip_x1) x1 = s->clip_x1;
        if (y1 > s->clip_y1) y1 = s->clip_y1;
    }
    for (int py = y0; py < y1 && x1 > x0; py++)
        memset(g_rec + (size_t)py * g_rec_w + x0, 0, (size_t)(x1 - x0));
}

// Blend black over a run so each pixel ends at the DARKER of what it
// has and `want` (from `cov`, or `alpha` throughout): the extra alpha
// e that takes (1-r) to (1-a) is (a-r)/(1-r). The run is already
// clipped to the surface and the clip by the caller.
static void blend_max(struct ugfx_surface *s, int x0, int py, int n,
                      const unsigned char *cov, int alpha) {
    if (!rec_ready(s)) {
        ugfx_blend_hspan(s, x0, py, n, 0x000000, cov, (uint8_t)alpha);
        return;
    }
    unsigned char eff[SHADOW_SPAN_MAX];
    unsigned char *rec = g_rec + (size_t)py * g_rec_w;
    for (int done = 0; done < n; done += SHADOW_SPAN_MAX) {
        int m = n - done < SHADOW_SPAN_MAX ? n - done : SHADOW_SPAN_MAX;
        int any = 0;
        for (int i = 0; i < m; i++) {
            int a = cov ? cov[done + i] : alpha;
            int r = rec[x0 + done + i];
            if (a <= r) { eff[i] = 0; continue; }
            eff[i] = (unsigned char)((a - r) * 255 / (255 - r));
            rec[x0 + done + i] = (unsigned char)a;
            any = 1;
        }
        if (any) ugfx_blend_hspan(s, x0 + done, py, m, 0x000000, eff, 0);
    }
}

// One row of a LEFT or RIGHT band, where the coverage varies along the
// run: built into `cov` and emitted as a single span. The run is at
// most the shadow's reach plus a corner radius wide.
// A HOLLOW shadow's body: the rect and its corner radius. A pixel inside
// it is shaded only by how far it lies OUTSIDE the body's arc.
static int g_hollow;
static int g_bx, g_by, g_bw, g_bh, g_br;

// How much of pixel (px, py) the rounded body covers, 0..255 -- sixteen
// samples, as wm_render.c's window corners count it.
static int body_cover(int px, int py) {
    if (px < g_bx || py < g_by || px >= g_bx + g_bw || py >= g_by + g_bh) return 0;
    int ix = px - g_bx < g_bx + g_bw - 1 - px ? px - g_bx : g_bx + g_bw - 1 - px;
    int iy = py - g_by < g_by + g_bh - 1 - py ? py - g_by : g_by + g_bh - 1 - py;
    int r = g_br;
    if (ix >= r || iy >= r) return 255;
    int in = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int cx = 8 * ix + 2 * sx + 1 - 8 * r, cy = 8 * iy + 2 * sy + 1 - 8 * r;
            if (cx * cx + cy * cy <= 64 * r * r) in++;
        }
    return in * 255 / 16;
}

static void outer_span(struct ugfx_surface *s, const struct tile *t,
                       int x0, int x1, int py,
                       int cx0, int cx1, int cy0, int cy1, unsigned char *cov) {
    int n = x1 - x0;
    if (n <= 0) return;
    if (n > SHADOW_SPAN_MAX) n = SHADOW_SPAN_MAX;
    int any = 0;
    for (int i = 0; i < n; i++) {
        int al = shadow_alpha(t, x0 + i, py, cx0, cx1, cy0, cy1);
        if (g_hollow && al) al = al * (255 - body_cover(x0 + i, py)) / 255;
        cov[i] = (unsigned char)al;
        any |= al;
    }
    if (any) blend_max(s, x0, py, n, cov, 0);
}

void wm_shadow_draw_hollow(int x, int y, int w, int h, int corner_r, enum wm_shadow_kind kind) {
    g_hollow = 1;
    g_bx = x; g_by = y; g_bw = w; g_bh = h; g_br = corner_r > 0 ? corner_r : 0;
    wm_shadow_draw(x, y, w, h, corner_r, kind);
    g_hollow = 0;
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
    // A POPUP'S SHADOW STAYS BELOW ITS TOP EDGE: a menu hangs from the
    // bar or the control that opened it, and a shadow reaching up over
    // that bar reads as the bar changing when the menu opens (a rounded
    // card tried every side, and menubar_test caught the next title
    // darkening). Windows' menus and Plasma's both keep it below.
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
    // The rows the window will paint over anyway, less its corner
    // boxes: skipped, since the rounded cut is the only place a
    // covered pixel shows through.
    int iy0 = y + cr, iy1 = y + h - cr;
    // HOLLOW: a see-through body would show every shaded pixel under it,
    // so the whole rect is skipped bar its corner boxes, and those are
    // shaded only outside the arc (body_cover()).
    if (g_hollow) { iy0 = y; iy1 = y + h; }

    // EACH ROW IS THREE SEGMENTS, and the long one is flat. Left and
    // right of the arc centres the coverage varies, so those are built
    // per pixel; BETWEEN them dx is 0, so the whole run shares one
    // alpha and blends as a span. The skipped interior [ix0, ix1) is
    // exactly that middle segment -- ix0 == cx0 and ix1 == cx1 + 1,
    // since sx == x -- so an interior row simply omits it.
    unsigned char cov[SHADOW_SPAN_MAX];
    int mid0 = cx0 < bx0 ? bx0 : cx0;
    int mid1 = cx1 + 1 > bx1 ? bx1 : cx1 + 1;
    int left1 = cx0 < bx1 ? cx0 : bx1;
    int right0 = cx1 + 1 > bx0 ? cx1 + 1 : bx0;

    for (int py = by0; py < by1; py++) {
        // The body's own columns of a side band, on a hollow row that is
        // not a corner's: under the glass, so not shaded.
        int side_in = g_hollow && py >= y + cr && py < y + h - cr;
        if (side_in) {
            outer_span(s, t, bx0, left1 < x ? left1 : x, py, cx0, cx1, cy0, cy1, cov);
            outer_span(s, t, right0 > x + w ? right0 : x + w, bx1, py, cx0, cx1, cy0, cy1, cov);
            continue;
        }
        outer_span(s, t, bx0, left1, py, cx0, cx1, cy0, cy1, cov);
        if (!(py >= iy0 && py < iy1) && mid1 > mid0) {
            int dy = py < cy0 ? cy0 - py : (py > cy1 ? py - cy1 : 0);
            int al = edge_alpha(t, dy - t->cr);
            if (al > 0) blend_max(s, mid0, py, mid1 - mid0, 0, al);
        }
        outer_span(s, t, right0, bx1, py, cx0, cx1, cy0, cy1, cov);
    }
    wm_shadow_cover(x, y, w, h);   // the body painted next is opaque
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
