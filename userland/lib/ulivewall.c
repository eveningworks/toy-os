// Live wallpapers -- see lib/ulivewall.h.
#include "lib/ulivewall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixed.h"
#include "rt/sys.h"
#include "ui/uapp.h"

// --- colour ------------------------------------------------------------

// Five colours each, ground to brightest, taken from the shipped picture
// of the same name (data/wallpapers) so a live background sits beside
// them. THE ORDER IS THE DESCRIPTORS' `colours` CHOICE LIST.
static const uint32_t PALS[][5] = {
    { 0x0b1730, 0x0e3a5e, 0x17506e, 0x20607a, 0x2e7489 },   // aurora
    { 0x1a1433, 0x3b2a5c, 0x6b3f6e, 0xa8586a, 0xd9826a },   // dusk
    { 0x140808, 0x3d1210, 0x7a2412, 0xb8461a, 0xe07a2e },   // ember
    { 0x16191f, 0x262b33, 0x363d48, 0x4a5361, 0x606b7c },   // slate
};
#define PAL_COUNT ((int)(sizeof PALS / sizeof PALS[0]))

#define CR(c) ((int)((c) >> 16) & 255)
#define CG(c) ((int)((c) >> 8) & 255)
#define CB(c) ((int)(c) & 255)

static uint32_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// `t` is 0..FX_ONE: 0 is all `a`.
static uint32_t mix(uint32_t a, uint32_t b, fx_t t) {
    if (t <= 0) return a;
    if (t >= FX_ONE) return b;
    return rgb(CR(a) + (int)(((CR(b) - CR(a)) * (int64_t)t) >> FX_SHIFT),
               CG(a) + (int)(((CG(b) - CG(a)) * (int64_t)t) >> FX_SHIFT),
               CB(a) + (int)(((CB(b) - CB(a)) * (int64_t)t) >> FX_SHIFT));
}

static uint32_t toward_white(uint32_t c, int pct) { return mix(c, 0xffffff, FX_ONE * pct / 100); }
static uint32_t toward_black(uint32_t c, int pct) { return mix(c, 0, FX_ONE * pct / 100); }

static fx_t clamp01(fx_t t) { return t < 0 ? 0 : t > FX_ONE ? FX_ONE : t; }

static fx_t sstep(fx_t a, fx_t b, fx_t x) {
    if (x <= a) return 0;
    if (x >= b) return FX_ONE;
    fx_t t = fx_div(x - a, b - a);
    return fx_mul(fx_mul(t, t), 3 * FX_ONE - 2 * t);
}

// Fixed-point constants written as the decimal they are.
#define FXD(milli) ((fx_t)((int64_t)(milli) * FX_ONE / 1000))

// THE EFFECT'S CLOCK AS A PHASE: `mturns` thousandths of a turn per
// second, wrapped to one turn. Computed from the 64-bit millisecond
// clock each time, so a wallpaper left running for weeks loses nothing
// to an accumulated fraction.
static fx_t phase(const struct ulivewall *w, int mturns) {
    uint64_t m = (uint64_t)(mturns < 0 ? -mturns : mturns);
    fx_t p = (fx_t)((w->t_ms * m * (uint64_t)FX_ONE / 1000000u) & (FX_ONE - 1));
    return mturns < 0 ? -p : p;
}

static uint32_t rnd(struct ulivewall *w) {
    uint32_t x = w->rng ? w->rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return w->rng = x;
}

#define GRID(w, i, j) ((w)->grid[(j) * ((w)->gw + 1) + (i)])
#define U(w, i) ((fx_t)((int64_t)(i) * FX_ONE / (w)->gw))
#define V(w, j) ((fx_t)((int64_t)(j) * FX_ONE / (w)->gh))

// --- the effects: each fills the corner grid ---------------------------

// The default picture's bands, moving: four soft-edged curves over a sky.
static void fx_aurora(struct ulivewall *w, const uint32_t *P) {
    static const fx_t base[4] = { FXD(300), FXD(420), FXD(550), FXD(680) };
    static const fx_t amp[4]  = { FXD(70), FXD(90), FXD(100), FXD(80) };
    static const fx_t fr1[4]  = { FXD(550), FXD(750), FXD(450), FXD(650) };   // turns across
    static const fx_t fr2[4]  = { FXD(1870), FXD(2550), FXD(1530), FXD(2210) };
    static const int rate[4]  = { 12, -10, 8, -14 };
    static const fx_t off[4]  = { 0, FXD(271), FXD(493), FXD(700) };
    uint32_t sky1 = rgb(CR(P[0]) + 8, CG(P[0]) + 12, CB(P[0]) + 16);
    for (int i = 0; i <= w->gw; i++) {
        fx_t u = U(w, i), ys[4];
        for (int k = 0; k < 4; k++) {
            fx_t p = phase(w, rate[k]);
            ys[k] = base[k] + fx_mul(amp[k], fx_sin(fx_mul(u, fr1[k]) + p + off[k]))
                  + fx_mul(FXD(25), fx_sin(fx_mul(u, fr2[k]) - p - p / 3 + 2 * off[k]));
        }
        for (int j = 0; j <= w->gh; j++) {
            fx_t v = V(w, j);
            uint32_t c = mix(P[0], sky1, v);
            for (int k = 0; k < 4; k++)
                c = mix(c, P[k + 1], fx_mul(sstep(ys[k] - FXD(50), ys[k] + FXD(50), v), FXD(920)));
            GRID(w, i, j) = c;
        }
    }
}

// Five colour fields drifting on rates that do not divide each other, so
// the picture never quite repeats -- Windows 11's and macOS's gradients.
static void fx_drift(struct ulivewall *w, const uint32_t *P) {
    static const int ord[5] = { 2, 3, 1, 0, 4 };
    fx_t bx[5], by[5];
    for (int b = 0; b < 5; b++) {
        bx[b] = FX_HALF + fx_mul(FXD(450), fx_sin(phase(w, 8 + 3 * b) + b * FXD(300)));
        by[b] = FX_HALF + fx_mul(FXD(450), fx_sin(phase(w, 6 + 2 * b) + b * FXD(430)));
    }
    fx_t aspect = (fx_t)((int64_t)w->gw * FX_ONE / (w->gh ? w->gh : 1));
    for (int j = 0; j <= w->gh; j++)
        for (int i = 0; i <= w->gw; i++) {
            fx_t u = U(w, i), v = V(w, j);
            int64_t sw = 0, r = 0, g = 0, bl = 0;
            for (int b = 0; b < 5; b++) {
                fx_t dx = fx_mul(u - bx[b], aspect), dy = v - by[b];
                fx_t d2 = fx_mul(dx, dx) + fx_mul(dy, dy);
                int64_t q = ((int64_t)FX_ONE << FX_SHIFT) / (d2 + FXD(30));
                int64_t wt = (q * q) >> FX_SHIFT;
                uint32_t c = P[ord[b]];
                sw += wt; r += wt * CR(c); g += wt * CG(c); bl += wt * CB(c);
            }
            GRID(w, i, j) = sw ? rgb((int)(r / sw), (int)(g / sw), (int)(bl / sw)) : P[0];
        }
}

// Swell under a night sky: five layers of slow sine hills, the nearer
// ones darker and quicker, with a light crest along each.
static void fx_tide(struct ulivewall *w, const uint32_t *P) {
    uint32_t layer[5];
    for (int k = 0; k < 5; k++) layer[k] = mix(P[3], toward_black(P[0], 40), FX_ONE * k / 4);
    fx_t e = FX_ONE / (w->gh ? w->gh : 1);
    if (e < FXD(4)) e = FXD(4);
    for (int i = 0; i <= w->gw; i++) {
        fx_t u = U(w, i), ys[5];
        for (int k = 0; k < 5; k++)
            ys[k] = FXD(460 + 110 * k)
                  + fx_mul(FXD(18 + 8 * k), fx_sin(fx_mul(u, FXD(1000 + 600 * k))
                                                   + phase(w, 16 + 10 * k) + k * FXD(200)));
        for (int j = 0; j <= w->gh; j++) {
            fx_t v = V(w, j);
            uint32_t c = mix(P[0], P[2], clamp01(v * 2));
            // EACH EDGE RAMPS ACROSS A CELL: a hard step at a grid point
            // interpolates into a staircase, one stair per cell. The crest
            // is the band just under it, a little lighter.
            for (int k = 0; k < 5; k++) {
                fx_t a = sstep(ys[k] - e, ys[k] + e, v);
                fx_t crest = a - sstep(ys[k] + e, ys[k] + 4 * e, v);
                c = mix(c, mix(layer[k], toward_white(layer[k], 18), crest < 0 ? 0 : crest), a);
            }
            GRID(w, i, j) = c;
        }
    }
}

// Metaballs: six blobs whose fields add, so two that meet merge.
static void fx_lava(struct ulivewall *w, const uint32_t *P) {
    fx_t bx[6], by[6], r2[6];
    fx_t aspect = (fx_t)((int64_t)w->gw * FX_ONE / (w->gh ? w->gh : 1));
    for (int b = 0; b < 6; b++) {
        bx[b] = fx_mul(FX_HALF + fx_mul(FXD(360), fx_sin(phase(w, 10 + 3 * b) + b * FXD(330))), aspect);
        by[b] = FX_HALF + fx_mul(FXD(420), fx_sin(phase(w, 7 + 2 * b) + b * FXD(210)));
        fx_t r = FXD(100 + 25 * (b % 3));
        r2[b] = fx_mul(r, r);
    }
    uint32_t edge = P[3], core = toward_white(P[4], 40);
    for (int j = 0; j <= w->gh; j++) {
        fx_t v = V(w, j);
        uint32_t bg = mix(P[0], P[1], v);
        for (int i = 0; i <= w->gw; i++) {
            fx_t x = fx_mul(U(w, i), aspect), f = 0;
            for (int b = 0; b < 6; b++) {
                fx_t dx = x - bx[b], dy = v - by[b];
                f += fx_div(r2[b], fx_mul(dx, dx) + fx_mul(dy, dy) + 7);
            }
            uint32_t blob = mix(edge, core, sstep(FXD(1100), FXD(2600), f));
            GRID(w, i, j) = mix(bg, blob, sstep(FXD(850), FXD(1100), f));
        }
    }
}

// A dusk gradient; the points are drawn over it by draw_fireflies().
static void fx_fireflies(struct ulivewall *w, const uint32_t *P) {
    uint32_t top = toward_black(P[0], 40);
    for (int j = 0; j <= w->gh; j++) {
        uint32_t c = mix(top, P[1], V(w, j));
        for (int i = 0; i <= w->gw; i++) GRID(w, i, j) = c;
    }
}

// The classic fire: heat fed along the bottom, averaged upward and cooled
// as it rises, one step per 1/24 s of effect time.
#define HEAT_MAX 4096
static void ember_step(struct ulivewall *w) {
    int gw = w->gw, gh = w->gh;
    uint16_t *h = w->heat;
    fx_t pa = phase(w, 111), pb = phase(w, -48);
    for (int x = 0; x < gw; x++) {
        fx_t n = FX_HALF + fx_mul(FX_HALF, fx_mul(fx_sin(x * 2189 + pa), fx_sin(x * 730 + pb)));
        int heat = (int)(((int64_t)n * HEAT_MAX) >> FX_SHIFT);
        if (rnd(w) % 10 >= 6) heat = heat * 2 / 5;
        h[gh * gw + x] = h[(gh + 1) * gw + x] = (uint16_t)heat;
    }
    // Cooling scaled to the grid's height, so the flames reach the same
    // fraction of the screen whatever the resolution.
    int decay = 49 * 135 / (gh ? gh : 1);
    if (decay < 1) decay = 1;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            int s = h[(y + 1) * gw + (x + gw - 1) % gw] + h[(y + 1) * gw + x]
                  + h[(y + 1) * gw + (x + 1) % gw] + h[(y + 2) * gw + x];
            int v = s / 4 - decay;
            h[y * gw + x] = (uint16_t)(v < 0 ? 0 : v);
        }
}

static void fx_ember(struct ulivewall *w, const uint32_t *P) {
    if (!w->heat) return;
    while (w->sim_ms + 42 <= w->t_ms) {
        // BEHIND BY MORE THAN A FEW STEPS (a pause, a stall): skip ahead
        // rather than spend a frame catching up on fire nobody saw.
        if (w->t_ms - w->sim_ms > 42 * 6) w->sim_ms = w->t_ms - 42 * 6;
        ember_step(w);
        w->sim_ms += 42;
    }
    const uint32_t stop[5] = { toward_black(P[0], 30), P[1], P[2], P[3], toward_white(P[4], 60) };
    static const fx_t at[5] = { 0, FXD(300), FXD(550), FXD(800), FX_ONE };
    for (int j = 0; j <= w->gh; j++)
        for (int i = 0; i <= w->gw; i++) {
            int hx = i < w->gw ? i : w->gw - 1, hy = j < w->gh ? j : w->gh - 1;
            fx_t v = clamp01((fx_t)((int64_t)w->heat[hy * w->gw + hx] * FX_ONE * 5 / 4 / HEAT_MAX));
            int k = 0;
            while (k < 3 && v > at[k + 1]) k++;
            GRID(w, i, j) = mix(stop[k], stop[k + 1], fx_div(v - at[k], at[k + 1] - at[k]));
        }
}

static const struct {
    const char *name;
    void (*grid)(struct ulivewall *w, const uint32_t *P);
} FX[] = {
    { "aurora",    fx_aurora },
    { "drift",     fx_drift },
    { "tide",      fx_tide },
    { "lava",      fx_lava },
    { "fireflies", fx_fireflies },
    { "ember",     fx_ember },
};
#define FX_COUNT ((int)(sizeof FX / sizeof FX[0]))

// --- painting -----------------------------------------------------------

// The grid to the screen, one bilinear pass: each cell row's left and
// right edge colours are found once, then the pixels between them are
// three adds apiece. Channels carry 8 fraction bits.
static void fill_bilerp(const struct ulivewall *w, struct ugfx_surface *s, int cell) {
    int stride = w->gw + 1;
    for (int j = 0; j < w->gh; j++) {
        int y0 = j * cell;
        for (int yy = 0; yy < cell && y0 + yy < s->h; yy++) {
            int fy = yy * 256 / cell;
            uint32_t *dst = s->pixels + (size_t)(y0 + yy) * (size_t)s->w;
            for (int i = 0; i < w->gw; i++) {
                int x0 = i * cell, cols = s->w - x0 < cell ? s->w - x0 : cell;
                if (cols <= 0) break;
                uint32_t a = w->grid[j * stride + i], b = w->grid[j * stride + i + 1];
                uint32_t c = w->grid[(j + 1) * stride + i], d = w->grid[(j + 1) * stride + i + 1];
                int lr = CR(a) * (256 - fy) + CR(c) * fy, rr = CR(b) * (256 - fy) + CR(d) * fy;
                int lg = CG(a) * (256 - fy) + CG(c) * fy, rg = CG(b) * (256 - fy) + CG(d) * fy;
                int lb = CB(a) * (256 - fy) + CB(c) * fy, rb = CB(b) * (256 - fy) + CB(d) * fy;
                int dr = (rr - lr) / cell, dg = (rg - lg) / cell, db = (rb - lb) / cell;
                uint32_t *p = dst + x0;
                for (int xx = 0; xx < cols; xx++) {
                    p[xx] = ((uint32_t)(lr >> 8) << 16) | ((uint32_t)(lg >> 8) << 8) | (uint32_t)(lb >> 8);
                    lr += dr; lg += dg; lb += db;
                }
            }
        }
    }
}

// Each point a soft disc, added over the ground. Brightness pulses on its
// own rate, so they do not blink together.
static void draw_fireflies(const struct ulivewall *w, struct ugfx_surface *s) {
    int R = s->h / 36;
    if (R < 2) R = 2;
    int r2 = R * R;
    for (int k = 0; k < ULIVEWALL_PARTS; k++) {
        const int32_t *q = w->part[k];
        fx_t pulse = fx_sin(phase(w, 140 * q[3] / FX_ONE + 20) + q[2]);
        int glow = 90 + (pulse > 0 ? (int)((165 * (int64_t)pulse) >> FX_SHIFT) : 0);   // of 255
        int cx = (int)(((int64_t)q[0] * s->w) >> FX_SHIFT), cy = (int)(((int64_t)q[1] * s->h) >> FX_SHIFT);
        for (int y = cy - R; y <= cy + R; y++) {
            if (y < 0 || y >= s->h) continue;
            uint32_t *row = s->pixels + (size_t)y * (size_t)s->w;
            for (int x = cx - R; x <= cx + R; x++) {
                if (x < 0 || x >= s->w) continue;
                int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (d2 >= r2) continue;
                int f = (r2 - d2) * 256 / r2;
                f = f * f / 256 * glow / 255;   // 0..256
                uint32_t c = row[x];
                row[x] = rgb(CR(c) + 255 * f / 256, CG(c) + 210 * f / 256, CB(c) + 120 * f / 256);
            }
        }
    }
}

// --- plain colours -------------------------------------------------------

static const struct { const char *word; uint32_t rgb; } PLAIN[] = {
    { "blue", 0x183c5a }, { "teal", 0x1d5b5b }, { "slate", 0x3a4350 },
    { "graphite", 0x26282c }, { "plum", 0x4a2d4f }, { "forest", 0x234a33 },
};

uint32_t ulivewall_colour(const char *word) {
    for (unsigned i = 0; i < sizeof PLAIN / sizeof PLAIN[0]; i++)
        if (word && !strcmp(word, PLAIN[i].word)) return PLAIN[i].rgb;
    return PLAIN[0].rgb;
}

// --- the effect's life --------------------------------------------------

int ulivewall_options(const char *name, struct usaver *out) {
    char desc[96], conf[96];
    snprintf(desc, sizeof desc, "%s/%s.wallpaper", LIVEWALL_DESC_DIR, name);
    ulivewall_conf_path(name, conf, sizeof conf);
    return usaver_load_files(name, desc, conf, out);
}

void ulivewall_conf_path(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.conf", LIVEWALL_CONF_DIR, name);
}

void ulivewall_reload(struct ulivewall *w, const char *name) {
    static struct usaver opts;   // past the 2 KB ring-3 frame cap
    ulivewall_options(name, &opts);
    w->speed = usaver_int(&opts, "speed", 4);
    if (w->speed < 1) w->speed = 1;
    if (w->speed > 10) w->speed = 10;
    w->palette = usaver_index(&opts, "colours", 0);
    if (w->palette < 0 || w->palette >= PAL_COUNT) w->palette = 0;
}

int ulivewall_open(struct ulivewall *w, const char *name) {
    memset(w, 0, sizeof *w);
    w->effect = -1;
    for (int i = 0; i < FX_COUNT; i++)
        if (!strcmp(FX[i].name, name)) w->effect = i;
    if (w->effect < 0) return 0;
    sys_getrandom(&w->rng, sizeof w->rng);
    for (int k = 0; k < ULIVEWALL_PARTS; k++) {
        w->part[k][0] = (int32_t)(rnd(w) % FX_ONE);
        w->part[k][1] = FXD(250) + (int32_t)(rnd(w) % FXD(730));
        w->part[k][2] = (int32_t)(rnd(w) % FX_ONE);
        w->part[k][3] = FXD(300) + (int32_t)(rnd(w) % FXD(600));
    }
    ulivewall_reload(w, name);
    return 1;
}

void ulivewall_close(struct ulivewall *w) {
    free(w->grid);
    free(w->heat);
    w->grid = 0;
    w->heat = 0;
}

void ulivewall_step(struct ulivewall *w, uint32_t dt_ms) {
    uint32_t dt = dt_ms * (uint32_t)w->speed / 4;
    w->t_ms += dt;
    if (w->effect < 0 || strcmp(FX[w->effect].name, "fireflies")) return;
    // Drift as a slow wander: each point's heading turns on its own rate.
    for (int k = 0; k < ULIVEWALL_PARTS; k++) {
        int32_t *q = w->part[k];
        fx_t a = phase(w, 33 * q[3] / FX_ONE + 5) + q[2];
        q[0] += (int32_t)(((int64_t)fx_sin(a) * FXD(16) / FX_ONE) * dt / 1000);
        q[1] += (int32_t)(((int64_t)fx_cos(a + q[2]) * FXD(17) / FX_ONE) * dt / 1000);
        q[0] &= FX_ONE - 1;
        if (q[1] < FXD(200)) q[1] = FXD(200);
        if (q[1] > FXD(980)) q[1] = FXD(980);
    }
}

void ulivewall_render(struct ulivewall *w, struct ugfx_surface *s, int cell) {
    if (w->effect < 0 || !s || s->w <= 0 || s->h <= 0) return;
    if (cell < 2) cell = 2;
    int gw = (s->w + cell - 1) / cell, gh = (s->h + cell - 1) / cell;
    if (gw != w->gw || gh != w->gh || !w->grid) {
        free(w->grid);
        free(w->heat);
        w->gw = gw;
        w->gh = gh;
        w->grid = malloc((size_t)(gw + 1) * (size_t)(gh + 1) * 4);
                w->heat = calloc((size_t)gw * (size_t)(gh + 2), 2);
        w->sim_ms = w->t_ms;
        if (!w->grid) return;
        // LIT AT ONCE, not over the next second: a cold fire is a black
        // screen, and a Settings preview renders exactly one frame.
        if (w->heat && FX[w->effect].grid == fx_ember)
            for (int i = 0; i < 48; i++) ember_step(w);
    }
    FX[w->effect].grid(w, PALS[w->palette]);
    fill_bilerp(w, s, cell);
    if (!strcmp(FX[w->effect].name, "fireflies")) draw_fireflies(w, s);
}

// --- a wallpaper program -----------------------------------------------

static struct ulivewall g_wall;
static const char *g_name;
static char g_conf[96];
static long long g_conf_gen = -1;
static uint64_t g_last_ns;

static int on_tick(struct uapp *a) {
    (void)a;
    uint64_t now = sys_monotonic_ns();
    uint64_t dt = g_last_ns ? (now - g_last_ns) / 1000000u : 0;
    g_last_ns = now;
    // AFTER A PAUSE (the compositor withholds the timer while the
    // desktop is covered) carry on from where it stopped, not jump.
    if (dt > 250) dt = 250;
    long long gen = sys_fs_generation_of(g_conf);
    if (gen != g_conf_gen) {
        g_conf_gen = gen;
        ulivewall_reload(&g_wall, g_name);
    }
    ulivewall_step(&g_wall, (uint32_t)dt);
    return 1;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    ulivewall_render(&g_wall, d->surface, ULIVEWALL_CELL);
}

int ulivewall_main(const char *name) {
    g_name = name;
    if (!ulivewall_open(&g_wall, name)) {
        fprintf(stderr, "%s: no such live wallpaper\n", name);
        return 1;
    }
    ulivewall_conf_path(name, g_conf, sizeof g_conf);
    g_conf_gen = sys_fs_generation_of(g_conf);
    // A SMALL FIRST WINDOW: the compositor proposes the screen's size at
    // once, and this is what it would otherwise allocate twice.
    struct uapp_desc desc = {
        .title = "Live wallpaper",
        .app_id = "wallpaper",
        .flags = UAPP_RESIZABLE,
        .w = 320, .h = 180,
        .tick_ms = 66,   // ~15 frames a second: smooth for slow motion, cheap
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    int rc = uapp_run(&desc);
    ulivewall_close(&g_wall);
    return rc;
}
