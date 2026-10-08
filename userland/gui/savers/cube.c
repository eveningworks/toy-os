// THE DESKTOP, FOLDED INTO A SOLID THAT TUMBLES AND BOUNCES. XScreenSaver's
// grab-the-screen hacks (flipscreen3d) and the Compiz cube, as one saver:
// the screen is captured as it starts, the first frame IS that capture, and
// it shrinks onto the solid's front face while the other faces swing out
// from behind and fold shut round it (mockup W1, 2026-10-06). The shape is
// an option -- a cube, a pyramid or a ball -- and the solid is lib/usolid.h.
//
// **THE CAPTURE IS TAKEN BEFORE THE WINDOW GOES FULLSCREEN, AND WITHOUT IT**
// (WIN_SHOT_NO_SELF): this window must never be in its own picture. A
// capture that fails -- -EBUSY while a fullscreen client holds the display
// -- leaves a grey checker on black with no intro, since there is nothing
// to fold.
//
// **IT TAKES DAMAGE** (the `damage` option, mockup D3, 2026-10-08): a hit
// chips the corner that struck -- on each face that meets there -- and
// throws debris that falls into a pile at the bottom (lib/upile.h). Each
// hit wears the solid a little smaller; at half size it shatters and a
// fresh one folds in. The pile is painted into the backdrop, so it costs
// nothing once it has settled.
//
// **ONLY WHAT MOVED IS REPAINTED.** Every frame restores the backdrop under
// the box the solid last occupied IN THAT BUFFER, then draws -- the toolkit
// rotates two or three buffers, so "last frame's box" would leave a trail
// in the one that was not drawn last. A buffer it has not seen gets the
// whole backdrop.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/ugfx_tex.h"
#include "ui/ulog.h"
#include "rt/sys.h"
#include "fixed.h"
#include "lib/usaver.h"
#include "lib/ushot.h"
#include "lib/usolid.h"
#include "lib/upile.h"
#include <stdlib.h>
#include <string.h>

// The intro, in milliseconds from the first full-screen frame.
#define HOLD_END    600    // the capture, untouched: the screen as it was
#define SHRINK_END 1600    // ...shrunk onto the front face
#define FOLD_END   3400    // ...and every face folded shut
#define RAMP_MS    1500    // spin and drift ease in after the fold
#define SQUASH_MS   220

// Where the solid has turned to by the time it closes, so the fold is seen
// from an angle rather than face-on, where a folding face is edge-on.
#define TILT_YAW   (FX_ONE * 32 / 360)
#define TILT_PITCH (FX_ONE * 22 / 360)

// Each hinge's window inside the fold, per shape, in usolid_stages() order.
struct window { int t0, t1; };
static const struct window FOLD[USOLID_SHAPES][USOLID_STAGE_MAX] = {
    [USOLID_CUBE]    = { {1600, 2800}, {1700, 2900}, {1800, 3000}, {1900, 3100}, {2300, 3400} },
    [USOLID_PYRAMID] = { {1600, 2800}, {1750, 2950}, {1900, 3100} },
    [USOLID_BALL]    = { {1600, 2700}, {2600, 3400} },
};

// Options (data/wm/savers/cube.saver).
static int g_shape = USOLID_CUBE;
static int g_size = 35;        // the cube's edge, % of the screen's shorter side
static int g_speed = 4;
static int g_spin = 4;
static int g_dimmed = 1;
static int g_intro = 1;
static int g_lit = 1;
static int g_squash = 1;
static int g_damage = 1;
enum { DEBRIS_SHARDS, DEBRIS_SAND, DEBRIS_CRUMBLE };
static int g_debris = DEBRIS_CRUMBLE;

// --- damage ---------------------------------------------------------------

#define WEAR_PER_HIT 965     // the size kept per hit, of 1000
#define SHATTER_AT   500     // ...and below this it bursts
#define GONE_MS     1600     // between the burst and the next fold
#define CHIPS_MAX     48
static int g_scale = 1000;   // of 1000
static uint64_t g_gone_until;
static struct upile g_pile;
static int g_pile_ready;
static unsigned g_hits;
// A chip: a corner of the cube (signs), the face it is on (its axis), and
// how big and how it is shaped.
static struct chip { int8_t sx, sy, sz, axis; uint8_t r, seed; } g_chips[CHIPS_MAX];
static int g_nchips;

static struct ushot g_shot;
static int g_have_shot;
static int g_sw, g_sh;
static struct ugfx_texture g_full;   // the capture
static struct ugfx_texture g_tex;    // ...scaled down for the faces
static uint32_t *g_back;             // the backdrop, screen size
static int g_back_rows;              // how much of it is made
static int *g_col0, *g_colw;         // the backdrop's bilinear columns

static struct usolid g_solid;
static int g_closed_built;
static struct usolid_motion g_mo;
static uint64_t g_t0, g_last;
static uint64_t g_sq_t;
static int g_sq_hit;
static int g_logged_phase;

#define BUFS 4
static struct {
    const uint32_t *px;
    struct usolid_box b;   // what to restore next time; screen coordinates
} g_buf[BUFS];
static int g_buf_next;

static uint64_t now_ms(void) { return sys_monotonic_ns() / 1000000ULL; }

static fx_t ease(int t, int t0, int t1) {
    if (t <= t0) return 0;
    if (t >= t1) return FX_ONE;
    int64_t u = (int64_t)(t - t0) * FX_ONE / (t1 - t0);
    if (u < FX_HALF) return (fx_t)(4 * u * u / FX_ONE * u / FX_ONE);
    int64_t w = 2 * FX_ONE - 2 * u;   // 0 .. 1
    return (fx_t)(FX_ONE - w * w / FX_ONE * w / FX_ONE / 2);
}

static int lerpi(int a, int b, fx_t k) { return a + (int)(((int64_t)(b - a) * k) >> FX_SHIFT); }

// --- the pictures -------------------------------------------------------

// The face texture: the capture box-filtered down to about 512 wide. A
// face is a few hundred pixels across, and point-sampling a 1920-wide
// capture into it shimmers as it turns.
static int make_texture(void) {
    int f = (g_sw + 511) / 512;
    if (f < 1) f = 1;
    int tw = g_sw / f, th = g_sh / f;
    uint32_t *px = malloc((size_t)tw * th * 4);
    if (!px) return 0;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            unsigned r = 0, g = 0, b = 0;
            for (int j = 0; j < f; j++) {
                const uint32_t *row = g_full.pixels + (size_t)(y * f + j) * g_sw + x * f;
                for (int i = 0; i < f; i++) {
                    r += (row[i] >> 16) & 0xFF; g += (row[i] >> 8) & 0xFF; b += row[i] & 0xFF;
                }
            }
            unsigned n = (unsigned)(f * f);
            px[(size_t)y * tw + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    g_tex.pixels = px;
    g_tex.w = tw;
    g_tex.h = th;
    return 1;
}

// THE BACKDROP IS MADE A SLICE PER TICK, during the hold, because the
// whole of it in on_open would put a blank frame in front of the first
// one -- which is the capture, and has to appear at once to be seamless.
// Dimmed: the face texture scaled back up bilinearly (which is the blur)
// at 3/8 brightness.
static void make_backdrop_rows(int rows) {
    if (g_back_rows >= g_sh) return;
    if (!g_dimmed) {
        memset(g_back, 0, (size_t)g_sw * g_sh * 4);
        g_back_rows = g_sh;
        return;
    }
    int tw = g_tex.w, th = g_tex.h;
    if (!g_col0) {
        g_col0 = malloc(sizeof(int) * (size_t)g_sw);
        g_colw = malloc(sizeof(int) * (size_t)g_sw);
        if (!g_col0 || !g_colw) { g_dimmed = 0; make_backdrop_rows(rows); return; }
        for (int x = 0; x < g_sw; x++) {
            int sx = (int)(((int64_t)(2 * x + 1) * tw * 128) / g_sw) - 128;   // 8.8
            if (sx < 0) sx = 0;
            if (sx > (tw - 1) * 256) sx = (tw - 1) * 256;
            g_col0[x] = sx >> 8;
            g_colw[x] = sx & 0xFF;
        }
    }
    int end = g_back_rows + rows;
    if (end > g_sh) end = g_sh;
    for (int y = g_back_rows; y < end; y++) {
        int sy = (int)(((int64_t)(2 * y + 1) * th * 128) / g_sh) - 128;
        if (sy < 0) sy = 0;
        if (sy > (th - 1) * 256) sy = (th - 1) * 256;
        int y0 = sy >> 8, wy = sy & 0xFF, y1 = y0 + 1 < th ? y0 + 1 : y0;
        const uint32_t *r0 = g_tex.pixels + (size_t)y0 * tw, *r1 = g_tex.pixels + (size_t)y1 * tw;
        uint32_t *out = g_back + (size_t)y * g_sw;
        for (int x = 0; x < g_sw; x++) {
            int x0 = g_col0[x], wx = g_colw[x], x1 = x0 + 1 < tw ? x0 + 1 : x0;
            uint32_t c = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int a = (r0[x0] >> sh) & 0xFF, b = (r0[x1] >> sh) & 0xFF;
                int cc = (r1[x0] >> sh) & 0xFF, d = (r1[x1] >> sh) & 0xFF;
                int top = a * 256 + (b - a) * wx, bot = cc * 256 + (d - cc) * wx;
                int v = (top * 256 + (bot - top) * wy) >> 16;
                c |= (uint32_t)(v * 3 / 8) << sh;
            }
            out[x] = c;
        }
    }
    g_back_rows = end;
}

static void use_fallback(void) {
    static uint32_t px[256 * 144];
    ugfx_texture_checker(px, 256, 144, 16, 0x5A5F66, 0x3A3E44);
    g_tex.pixels = px;
    g_tex.w = 256;
    g_tex.h = 144;
    g_intro = 0;
    g_dimmed = 0;
}

// A new screen size: the backdrop, the buffers' boxes and the centre all
// start again. Once for a capture; for the fallback, whatever size the
// window is, since without a capture nothing says what the screen's is.
static int adopt(int w, int h) {
    g_sw = w;
    g_sh = h;
    free(g_back);
    g_back = malloc((size_t)w * h * 4);
    g_back_rows = 0;
    memset(g_buf, 0, sizeof g_buf);
    g_mo.x256 = (w / 2) << 8;
    g_mo.y256 = (h / 2) << 8;
    g_mo.vx = g_speed * (w < h ? w : h) / 20;   // px/s: 4 is a fifth of the screen a second
    g_mo.vy = g_mo.vx * 7 / 10;                 // not 1:1, or it only ever finds two corners
    return g_back != NULL;
}

// --- per-buffer repair --------------------------------------------------

static struct usolid_box *buf_box(const struct ugfx_surface *s) {
    for (int i = 0; i < BUFS; i++)
        if (g_buf[i].px == s->pixels) return &g_buf[i].b;
    int i = g_buf_next;
    g_buf_next = (g_buf_next + 1) % BUFS;
    g_buf[i].px = s->pixels;
    g_buf[i].b = (struct usolid_box){ 0, 0, g_sw, g_sh };
    return &g_buf[i].b;
}

static void clip_box(struct usolid_box *b) {
    if (b->x0 < 0) b->x0 = 0;
    if (b->y0 < 0) b->y0 = 0;
    if (b->x1 > g_sw) b->x1 = g_sw;
    if (b->y1 > g_sh) b->y1 = g_sh;
}

static void restore(struct ugfx_surface *s, struct usolid_box b) {
    clip_box(&b);
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    for (int y = b.y0; y < b.y1; y++)
        memcpy(s->pixels + (size_t)y * s->w + b.x0, g_back + (size_t)y * g_sw + b.x0,
               (size_t)(b.x1 - b.x0) * 4);
    ugfx_mark_dirty_rect(s, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
}

// --- the frames ---------------------------------------------------------

static struct usolid_view view_at(int t) {
    int unit = g_size * (g_sw < g_sh ? g_sw : g_sh) / 200 * g_scale / 1000;
    if (unit < 8) unit = 8;
    struct usolid_view v = { 0, 0, 0, unit, unit * 8, g_lit };
    fx_t tilt = ease(t, SHRINK_END, FOLD_END);
    int64_t st = t - FOLD_END;
    // Ease in: the distance a body covers accelerating to full rate over
    // RAMP_MS, so the spin starts from still rather than with a jolt.
    int64_t ramp = st <= 0 ? 0 : st < RAMP_MS ? st * st / (2 * RAMP_MS) : st - RAMP_MS / 2;
    int64_t spin = (int64_t)FX_ONE * 10 * g_spin * ramp / 360000;   // 10 deg/s per step
    v.yaw   = (fx_t)((fx_mul(TILT_YAW, tilt) + spin) & (FX_ONE - 1));
    v.pitch = (fx_t)((fx_mul(TILT_PITCH, tilt) + spin * 4 / 7) & (FX_ONE - 1));
    v.roll  = (fx_t)((spin / 4) & (FX_ONE - 1));
    return v;
}

static void say_once(int phase, const char *what) {
    if (g_logged_phase >= phase) return;
    g_logged_phase = phase;
    ulogf("cube: %s\n", what);
}

// The capture, shrinking onto the front face. Positions AND texture
// coordinates move together, so the pyramid's front -- a triangle cut from
// the middle -- is reached by its top corners closing on the apex rather
// than by the picture squeezing.
static void draw_flat(struct ugfx_surface *s, int t, struct usolid_box *out) {
    say_once(1, "intro: shrink");
    fx_t k = ease(t, HOLD_END, SHRINK_END);
    struct usolid_view v = view_at(SHRINK_END);   // stage zero, face-on
    v.yaw = v.pitch = v.roll = 0;
    struct usolid_vert q[4];
    usolid_front(g_shape, g_tex.w, g_tex.h, q);
    static const int SX[4] = { 0, 1, 1, 0 }, SY[4] = { 0, 0, 1, 1 };
    struct ugfx_texvert tv[4];
    *out = (struct usolid_box){ g_sw, g_sh, 0, 0 };
    for (int i = 0; i < 4; i++) {
        struct usolid_proj p = usolid_project_pt(&v, q[i].p);
        tv[i].x = lerpi(SX[i] * g_sw, g_sw / 2 + p.x, k);
        tv[i].y = lerpi(SY[i] * g_sh, g_sh / 2 + p.y, k);
        tv[i].z = v.dist;   // flat and face-on: one depth, so affine is exact
        tv[i].u = lerpi(SX[i] * (g_sw - 1), q[i].u * (g_sw - 1) / (g_tex.w - 1), k);
        tv[i].v = lerpi(SY[i] * (g_sh - 1), q[i].v * (g_sh - 1) / (g_tex.h - 1), k);
        tv[i].shade = 255;
        if (tv[i].x < out->x0) out->x0 = tv[i].x;
        if (tv[i].y < out->y0) out->y0 = tv[i].y;
        if (tv[i].x > out->x1) out->x1 = tv[i].x;
        if (tv[i].y > out->y1) out->y1 = tv[i].y;
    }
    ugfx_tri3d(s, NULL, &g_full, 0, &tv[0], &tv[1], &tv[2]);
    ugfx_tri3d(s, NULL, &g_full, 0, &tv[0], &tv[2], &tv[3]);
}

// --- damage -----------------------------------------------------------------

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int rnd_in(int lo, int hi) { return hi <= lo ? lo : lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static uint32_t texel(int u, int v) {
    u = u < 0 ? 0 : u >= g_tex.w ? g_tex.w - 1 : u;
    v = v < 0 ? 0 : v >= g_tex.h ? g_tex.h - 1 : v;
    return g_tex.pixels[(size_t)v * g_tex.w + u];
}

// The corner that struck: the projected corner furthest toward the wall.
static int contact_vertex(int hit) {
    int best = 0, bv = -(1 << 30);
    for (int i = 0; i < g_solid.nv; i++) {
        const struct usolid_proj *p = &g_solid.pv[i];
        int d = (hit & USOLID_HIT_LEFT) ? -p->x : (hit & USOLID_HIT_RIGHT) ? p->x
              : (hit & USOLID_HIT_TOP) ? -p->y : p->y;
        if (d > bv) { bv = d; best = i; }
    }
    return best;
}

// A CHIP ON EACH FACE THAT MEETS AT THE CORNER, so the corner reads as
// broken. A corner hit again deepens its chips rather than adding more.
static void add_chips(struct geom_pt3 p) {
    if (g_shape != USOLID_CUBE) return;   // the cube's faces are what a chip is clipped to
    int8_t sx = p.x > 0 ? 1 : -1, sy = p.y > 0 ? 1 : -1, sz = p.z > 0 ? 1 : -1;
    for (int axis = 0; axis < 3; axis++) {
        int i;
        for (i = 0; i < g_nchips; i++) {
            struct chip *c = &g_chips[i];
            if (c->sx == sx && c->sy == sy && c->sz == sz && c->axis == axis) {
                if (c->r < 60) c->r = (uint8_t)(c->r + 6);
                break;
            }
        }
        if (i < g_nchips || g_nchips >= CHIPS_MAX) continue;
        g_chips[g_nchips++] = (struct chip){ sx, sy, sz, (int8_t)axis,
                                             (uint8_t)rnd_in(16, 30), (uint8_t)rnd() };
    }
}

static fx_t *axis_of(struct geom_pt3 *p, int a) { return a == 0 ? &p->x : a == 1 ? &p->y : &p->z; }

// Each chip a jagged dark crater with a lit rim, laid ON ITS FACE -- so it
// turns and foreshortens with it -- and clipped to the face's square. A
// face turned away hides its chips, which is all the occlusion a convex
// solid needs.
static void draw_chips(struct ugfx_surface *s, const struct usolid_view *v, int cx, int cy,
                       fx_t sx, fx_t sy) {
    for (int i = 0; i < g_nchips; i++) {
        const struct chip *c = &g_chips[i];
        int8_t sign[3] = { c->sx, c->sy, c->sz };
        int a = c->axis, b = (a + 1) % 3, d = (a + 2) % 3;
        fx_t r = (fx_t)((int64_t)FX_ONE * c->r / 100);
        fx_t off = r * 45 / 100;
        struct geom_pt3 ctr = { 0, 0, 0 }, out;
        *axis_of(&ctr, a) = sign[a] * FX_ONE;
        *axis_of(&ctr, b) = sign[b] * (FX_ONE - off);
        *axis_of(&ctr, d) = sign[d] * (FX_ONE - off);
        out = ctr;
        *axis_of(&out, a) = sign[a] * (FX_ONE + FX_ONE / 4);
        if (usolid_project_pt(v, out).z >= usolid_project_pt(v, ctr).z) continue;   // faces away
        int xs[10], ys[10];
        for (int k = 0; k < 10; k++) {
            fx_t t = (fx_t)(FX_ONE * k / 10);
            fx_t rr = fx_mul(r, FX_ONE * 65 / 100 +
                                fx_mul(FX_ONE * 35 / 100, fx_sin((fx_t)(c->seed * 257 + k * 9800))));
            struct geom_pt3 q = ctr;
            fx_t *qb = axis_of(&q, b), *qd = axis_of(&q, d);
            *qb += fx_mul(rr, fx_cos(t));
            *qd += fx_mul(rr, fx_sin(t));
            if (*qb > FX_ONE) *qb = FX_ONE;
            if (*qb < -FX_ONE) *qb = -FX_ONE;
            if (*qd > FX_ONE) *qd = FX_ONE;
            if (*qd < -FX_ONE) *qd = -FX_ONE;
            struct usolid_proj pp = usolid_project_pt(v, q);
            xs[k] = cx + fx_mul(pp.x, sx);
            ys[k] = cy + fx_mul(pp.y, sy);
        }
        ugfx_fill_polygon(s, xs, ys, 10, 0x121418);
        ugfx_draw_polyline(s, xs, ys, 10, 1, 0x7d838e, GEOM_AA);
    }
}

// Debris from the corner that struck, thrown back off the wall.
static void throw_debris(int hit, int vi, int cx, int cy) {
    const struct usolid_proj *p = &g_solid.pv[vi];
    int x = cx + p->x, y = cy + p->y;
    int dx = (hit & USOLID_HIT_LEFT) ? 1 : (hit & USOLID_HIT_RIGHT) ? -1 : 0;
    int dy = (hit & USOLID_HIT_TOP) ? 1 : (hit & USOLID_HIT_BOTTOM) ? -1 : 0;
    int base = g_sh / 4, unit = g_size * g_sh / 200 * g_scale / 1000;
    int sand = g_debris == DEBRIS_SAND;
    int n = sand ? 60 : 12;
    for (int i = 0; i < n; i++) {
        int vx = dx * rnd_in(base / 2, base * 3 / 2) + rnd_in(-base / 2, base / 2) + g_mo.vx / 5;
        int vy = dy * rnd_in(base / 2, base * 3 / 2) + rnd_in(-base / 2, base / 3) - base / 3;
        uint32_t col = texel(g_solid.v[vi].u + rnd_in(-24, 24), g_solid.v[vi].v + rnd_in(-24, 24));
        upile_add(&g_pile, sand ? UPILE_GRAIN : UPILE_SHARD, x, y, vx, vy,
                  sand ? g_pile.cell : rnd_in(unit / 16 + 2, unit / 8 + 3), col);
    }
}

// Worn to half its size: it bursts, and a fresh solid folds in after.
static void shatter(const struct usolid_box *box, int cx, int cy, uint64_t now) {
    int sand = g_debris == DEBRIS_SAND, unit = g_size * g_sh / 200 * g_scale / 1000;
    for (int i = 0; i < (sand ? 220 : 110); i++) {
        int x = cx + rnd_in(box->x0, box->x1), y = cy + rnd_in(box->y0, box->y1);
        int vx = (x - cx) * 3 + rnd_in(-g_sh / 8, g_sh / 8);
        int vy = (y - cy) * 3 - rnd_in(0, g_sh / 3);
        upile_add(&g_pile, sand ? UPILE_GRAIN : UPILE_SHARD, x, y, vx, vy,
                  sand ? g_pile.cell : rnd_in(unit / 14 + 2, unit / 6 + 3),
                  texel(rnd_in(0, g_tex.w - 1), rnd_in(0, g_tex.h - 1)));
    }
    g_gone_until = now + GONE_MS;
    g_nchips = 0;
    g_scale = 1000;
    ulogf("cube: shattered after %u hits\n", g_hits);
}

// What a hit does, once the solid is where it bounced to.
static void take_hit(int hit, const struct usolid_box *box, int cx, int cy, uint64_t now) {
    g_hits++;
    int vi = contact_vertex(hit);
    add_chips(g_solid.v[vi].p);
    throw_debris(hit, vi, cx, cy);
    g_scale = g_scale * WEAR_PER_HIT / 1000;
    if (g_hits <= 3 || g_hits % 10 == 0)
        ulogf("cube: hit %u chips %d debris %d scale %d\n", g_hits, g_nchips, g_pile.n, g_scale);
    if (g_scale < SHATTER_AT) shatter(box, cx, cy, now);
}

static void draw_solid(struct ugfx_surface *s, int t, int dt, struct usolid_box *out) {
    if (t < FOLD_END) {
        say_once(2, "intro: fold");
        fx_t stage[USOLID_STAGE_MAX];
        for (int i = 0; i < usolid_stages(g_shape); i++)
            stage[i] = ease(t, FOLD[g_shape][i].t0, FOLD[g_shape][i].t1);
        usolid_build(&g_solid, g_shape, stage, g_tex.w, g_tex.h);
        g_closed_built = 0;
    } else if (!g_closed_built) {
        say_once(3, "closed, bouncing");
        usolid_build(&g_solid, g_shape, NULL, g_tex.w, g_tex.h);
        g_closed_built = 1;
    }
    struct usolid_view v = view_at(t);
    struct usolid_box box;
    usolid_project(&g_solid, &v, &box);

    int hit = 0;
    if (t > FOLD_END) {
        int st = t - FOLD_END;
        int moved = st >= RAMP_MS ? dt : dt * st / RAMP_MS;
        // THE FLOOR IS THE PILE'S TOP under the solid, not the screen's
        // edge -- it lands on what it has shed.
        int floor = g_sh;
        if (g_pile_ready) {
            int x = g_mo.x256 >> 8;
            floor = upile_floor(&g_pile, x + box.x0, x + box.x1);
            if (floor < box.y1 - box.y0 + 2) floor = box.y1 - box.y0 + 2;
        }
        hit = usolid_bounce(&g_mo, &box, g_sw, floor, moved);
        if (hit && g_squash) { g_sq_t = (uint64_t)t; g_sq_hit = hit; }
    }
    int cx = g_mo.x256 >> 8, cy = g_mo.y256 >> 8;
    if (hit && g_damage && g_pile_ready) take_hit(hit, &box, cx, cy, now_ms());

    // THE SQUASH: flattened against the edge it hit and bulged along it,
    // over SQUASH_MS, with the side that touched held at the edge.
    fx_t sx = FX_ONE, sy = FX_ONE;
    if (g_sq_hit && (uint64_t)t < g_sq_t + SQUASH_MS) {
        fx_t a = fx_mul(FX_ONE / 5, fx_sin((fx_t)((t - g_sq_t) * (FX_ONE / 2) / SQUASH_MS)));
        if (g_sq_hit & (USOLID_HIT_LEFT | USOLID_HIT_RIGHT)) { sx = FX_ONE - a; sy = FX_ONE + a / 2; }
        else                                                 { sy = FX_ONE - a; sx = FX_ONE + a / 2; }
        if (g_sq_hit & USOLID_HIT_LEFT)   cx += fx_mul(box.x0, FX_ONE - sx);
        if (g_sq_hit & USOLID_HIT_RIGHT)  cx += fx_mul(box.x1, FX_ONE - sx);
        if (g_sq_hit & USOLID_HIT_TOP)    cy += fx_mul(box.y0, FX_ONE - sy);
        if (g_sq_hit & USOLID_HIT_BOTTOM) cy += fx_mul(box.y1, FX_ONE - sy);
    }
    usolid_draw(s, &g_solid, &g_tex, cx, cy, sx, sy);
    if (g_nchips) draw_chips(s, &v, cx, cy, sx, sy);
    // A pixel either side: the rasteriser's edges round outward of the
    // projected corners by up to one.
    out->x0 = cx + fx_mul(box.x0, sx) - 2;
    out->y0 = cy + fx_mul(box.y0, sy) - 2;
    out->x1 = cx + fx_mul(box.x1, sx) + 3;
    out->y1 = cy + fx_mul(box.y1, sy) + 3;
}

static void grow_box(struct usolid_box *b, int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    if (b->x1 <= b->x0 || b->y1 <= b->y0) { *b = (struct usolid_box){ x0, y0, x1, y1 }; return; }
    if (x0 < b->x0) b->x0 = x0;
    if (y0 < b->y0) b->y0 = y0;
    if (x1 > b->x1) b->x1 = x1;
    if (y1 > b->y1) b->y1 = y1;
}

// The pile, once the backdrop it is painted into is whole; then a step of
// it, and what it painted repaired in EVERY buffer, each when next drawn.
static void pile_tick(int t, int dt) {
    if (g_damage && !g_pile_ready && g_back_rows >= g_sh && t > FOLD_END) {
        int cell = g_sh / 360;
        if (cell < 2) cell = 2;
        if (upile_init(&g_pile, g_back, g_sw, g_sh, cell)) {
            g_pile.crumble = g_debris == DEBRIS_CRUMBLE;
            g_pile.sink_at = g_sh / 3;
            g_pile_ready = 1;
        }
    }
    if (!g_pile_ready) return;
    upile_step(&g_pile, dt);
    int x0, y0, x1, y1;
    if (upile_take_dirty(&g_pile, &x0, &y0, &x1, &y1))
        for (int i = 0; i < BUFS; i++)
            if (g_buf[i].px) grow_box(&g_buf[i].b, x0, y0, x1, y1);
}

// A FRESH SOLID after a burst: folded in again from the flat face, or --
// with the wrap animation off -- straight in, as the saver starts.
static void refold(uint64_t now) {
    g_gone_until = 0;
    g_mo.x256 = (g_sw / 2) << 8;
    g_mo.y256 = (g_sh / 3) << 8;
    g_closed_built = 0;
    g_sq_hit = 0;
    g_t0 = g_intro ? now - SHRINK_END : now - (FOLD_END + RAMP_MS);
    ulogf("cube: refolded\n");
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    // NOT FULLSCREEN YET: the window opens at its desc size and is resized
    // by the compositor's answer. The intro's clock starts at the first
    // frame that fills the screen, or its first 600 ms would be spent in a
    // window nobody saw.
    if (g_tex.pixels && !g_have_shot && (s->w != g_sw || s->h != g_sh)) adopt(s->w, s->h);
    if (!g_tex.pixels || !g_back || s->w != g_sw || s->h != g_sh) {
        ugfx_fill(s, 0);
        return;
    }
    uint64_t now = now_ms();
    if (!g_t0) {
        g_t0 = g_last = now;
        if (!g_intro) g_t0 -= FOLD_END + RAMP_MS;   // straight in, at speed
    }
    int dt = (int)(now - g_last);
    if (dt > 100) dt = 100;   // a stalled frame is not a reason to teleport
    g_last = now;
    int t = (int)(now - g_t0);
    // The backdrop is still being made: hold on the capture until it is.
    if (g_back_rows < g_sh && t > HOLD_END) {
        g_t0 = now - HOLD_END;
        t = HOLD_END;
    }

    if (g_gone_until && now >= g_gone_until) {
        refold(now);
        t = (int)(now - g_t0);
    }

    struct usolid_box *last = buf_box(s);
    struct usolid_box box = { 0, 0, 0, 0 };
    if (t < HOLD_END) {
        memcpy(s->pixels, g_full.pixels, (size_t)g_sw * g_sh * 4);
        ugfx_mark_dirty_rect(s, 0, 0, g_sw, g_sh);
        *last = (struct usolid_box){ 0, 0, g_sw, g_sh };
        return;
    }
    pile_tick(t, dt);
    restore(s, *last);
    if (g_gone_until) {
        // Between a burst and the next fold: only the debris.
    } else if (t < SHRINK_END) {
        draw_flat(s, t, &box);
    } else {
        draw_solid(s, t, dt, &box);
    }
    box.x1 += 1;
    box.y1 += 1;
    if (g_pile_ready && g_pile.n) {
        int x0, y0, x1, y1;
        upile_draw(&g_pile, s, &x0, &y0, &x1, &y1);
        grow_box(&box, x0, y0, x1, y1);
    }
    clip_box(&box);
    *last = box;
}

static int on_tick(struct uapp *a) {
    (void)a;
    if (g_back) make_backdrop_rows(g_sh / 12 + 1);
    return 1;
}

static void on_open(struct uapp *a) {
    // THE CAPTURE FIRST, before the window covers anything -- and without
    // this window in it either way.
    int rc = ushot_open_on(&g_shot, uapp_wmchan());
    if (rc == 0) rc = ushot_take(&g_shot, WIN_SHOT_SCREEN, WIN_SHOT_NO_SELF, 0, 0, 0, 0);
    if (rc == 0) {
        g_sw = g_shot.w;
        g_sh = g_shot.h;
        g_full = (struct ugfx_texture){ g_shot.px, g_shot.w, g_shot.h };
        g_have_shot = make_texture();
    }
    if (g_have_shot && adopt(g_shot.w, g_shot.h)) {
        ulogf("cube: captured %dx%d, texture %dx%d\n", g_sw, g_sh, g_tex.w, g_tex.h);
    } else {
        ulogf("cube: no capture (%s), drawing a checker\n",
              rc < 0 ? ushot_strerror(rc) : "out of memory");
        g_have_shot = 0;
        use_fallback();
    }
    uapp_set_fullscreen(a, 1);
}

static void load_options(void) {
    static struct usaver cfg;   // past the 2 KB ring-3 frame cap
    usaver_load("cube", &cfg);
    g_shape  = usaver_index(&cfg, "shape", USOLID_CUBE);
    g_size   = usaver_int(&cfg, "size", g_size);
    g_speed  = usaver_int(&cfg, "speed", g_speed);
    g_spin   = usaver_int(&cfg, "spin", g_spin);
    g_dimmed = strcmp(usaver_str(&cfg, "backdrop", "dimmed"), "black") != 0;
    g_intro  = strcmp(usaver_str(&cfg, "intro", "on"), "off") != 0;
    g_lit    = strcmp(usaver_str(&cfg, "lighting", "shaded"), "flat") != 0;
    g_squash = strcmp(usaver_str(&cfg, "squash", "on"), "off") != 0;
    g_damage = strcmp(usaver_str(&cfg, "damage", "on"), "off") != 0;
    const char *debris = usaver_str(&cfg, "debris", "crumble");
    g_debris = !strcmp(debris, "shards") ? DEBRIS_SHARDS : !strcmp(debris, "sand") ? DEBRIS_SAND
             : DEBRIS_CRUMBLE;
    sys_getrandom(&g_rng, sizeof g_rng);
    if (g_shape < 0 || g_shape >= USOLID_SHAPES) g_shape = USOLID_CUBE;
}

int main(void) {
    load_options();
    struct uapp_desc desc = {
        .title = "Cube",
        .app_id = "saver-cube",
        .flags = UAPP_RESIZABLE,
        .w = 640, .h = 480,
        .tick_ms = 33,
        .on_open = on_open,
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
