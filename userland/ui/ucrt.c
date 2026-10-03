// See ucrt.h. The passes, in the order they run over the rect:
//
//   copy     the rect as the app drew it, into `buf`
//   glow     `buf` at half resolution, box-blurred twice (close to a
//            Gaussian), screen-blended back -- phosphor bloom
//   compose  per pixel: glow, then the mask's channel gains, the
//            scanline and vignette gains (both SEPARABLE tables, so a
//            pixel costs multiplies, not a distance), flicker and noise
//   curve    each output pixel samples `buf` bilinearly at the point the
//            precomputed `map` names, or is bezel
//
// Everything is integer except building the tables, which happens when
// the rect's size or the curve changes and never per frame.
#include "ui/ucrt.h"

#include <stdlib.h>
#include <string.h>

#define UCRT_BEZEL 0xFFFFFFFFu
#define FIX 16            // the map's sub-pixel steps (12.4 fixed)
#define MAP_MAX 4095      // 12 integer bits; a wider rect is drawn flat

const struct ucrt_look ucrt_presets[UCRT_PRESET_COUNT] = {
    [UCRT_PRESET_SUBTLE]  = { 1, 1, 1, UCRT_CURVE_OFF,    UCRT_MASK_OFF,      0, 0 },
    [UCRT_PRESET_CLASSIC] = { 2, 2, 2, UCRT_CURVE_SUBTLE, UCRT_MASK_OFF,      0, 0 },
    [UCRT_PRESET_CURVED]  = { 3, 3, 3, UCRT_CURVE_STRONG, UCRT_MASK_APERTURE, 0, 0 },
};
const char *const ucrt_preset_names[UCRT_PRESET_COUNT] = { "Subtle", "Classic CRT", "Curved" };

// Strengths per level, out of 256.
static const int SCAN[UCRT_LEVEL_MAX + 1]  = { 0, 40, 77, 115 };
static const int GLOW[UCRT_LEVEL_MAX + 1]  = { 0, 90, 170, 230 };
static const int VIG[UCRT_LEVEL_MAX + 1]   = { 0, 64, 115, 166 };
static const int MASK_DIM = 56;

// The tube: barrel strength, the bezel's width and the glass's corner
// radius, the last two as fractions of the rect's height.
static const struct { float k, inset, radius; } CURVE[UCRT_CURVE_COUNT] = {
    { 0.0f, 0.0f, 0.0f }, { 0.045f, 1.0f / 37, 1.0f / 20 }, { 0.11f, 1.0f / 29, 1.0f / 13 },
};

int ucrt_preset_of(const struct ucrt_look *l) {
    for (int i = 0; i < UCRT_PRESET_COUNT; i++)
        if (!memcmp(&ucrt_presets[i], l, sizeof *l)) return i;
    return -1;
}

int ucrt_look_on(const struct ucrt_look *l) {
    return l->scanlines || l->glow || l->vignette || l->curve || l->mask || l->flicker || l->noise;
}

int ucrt_animates(const struct ucrt_look *l) { return l->flicker || l->noise; }

void ucrt_init(struct ucrt *c) {
    memset(c, 0, sizeof *c);
    c->period = 3;
    c->bezel = 0x0c0c0c;
    c->rng = 0x9e3779b9u;
}

void ucrt_free(struct ucrt *c) {
    free(c->map);
    free(c->buf);
    free(c->half);
    free(c->half2);
    c->map = c->buf = c->half = c->half2 = 0;
    c->w = c->h = 0;
}

// --- the curve ---------------------------------------------------------

// Where on the source a point of the glass shows from: crt-geom's
// uv * (1 + k r^2), scaled so each edge's midpoint stays on its edge.
static void warp(int curve, int w, int h, float x, float y, float *sx, float *sy) {
    float in = CURVE[curve].inset * h, gw = w - 2 * in, gh = h - 2 * in;
    float u = (x - in) / gw * 2 - 1, v = (y - in) / gh * 2 - 1;
    float k = CURVE[curve].k, f = (1 + k * (u * u + v * v)) / (1 + k);
    *sx = (u * f + 1) / 2 * w;
    *sy = (v * f + 1) / 2 * h;
}

// Inside the glass's rounded rect?
static int on_glass(int curve, int w, int h, int x, int y) {
    float in = CURVE[curve].inset * h, r = CURVE[curve].radius * h;
    float x0 = in + r, x1 = w - in - r, y0 = in + r, y1 = h - in - r;
    float fx = x + 0.5f, fy = y + 0.5f;
    if (fx < in || fx > w - in || fy < in || fy > h - in) return 0;
    float dx = fx < x0 ? x0 - fx : fx > x1 ? fx - x1 : 0;
    float dy = fy < y0 ? y0 - fy : fy > y1 ? fy - y1 : 0;
    return dx * dx + dy * dy <= r * r;
}

int ucrt_margin(const struct ucrt_look *l, int w, int h) {
    if (!l->curve || w < 8 || h < 8) return 0;
    // The glass's corner, 45 degrees round its arc, is the source point
    // nearest the source's corner that still shows: everything between
    // it and the corner is cut off.
    float in = CURVE[l->curve].inset * h, r = CURVE[l->curve].radius * h;
    float d = in + r * (1 - 0.7071f), sx, sy;
    warp(l->curve, w, h, d, d, &sx, &sy);
    int m = (int)(sx > sy ? sx : sy) + 2;
    return m > 0 ? m : 0;
}

static int build_map(struct ucrt *c) {
    if (c->w > MAP_MAX || c->h > MAP_MAX) return -1;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t *m = &c->map[y * c->w + x];
            float sx, sy;
            warp(c->look.curve, c->w, c->h, x + 0.5f, y + 0.5f, &sx, &sy);
            sx -= 0.5f;
            sy -= 0.5f;
            if (!on_glass(c->look.curve, c->w, c->h, x, y) || sx < 0 || sy < 0 ||
                sx > c->w - 1 || sy > c->h - 1) {
                *m = UCRT_BEZEL;
                continue;
            }
            *m = ((uint32_t)(sx * FIX) << 16) | (uint32_t)(sy * FIX);
        }
    c->map_curve = c->look.curve;
    return 0;
}

int ucrt_source_point(const struct ucrt *c, int px, int py, int *sx, int *sy) {
    *sx = px;
    *sy = py;
    if (!c->look.curve || !c->map || c->map_curve != c->look.curve) return 1;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= c->w) px = c->w - 1;
    if (py >= c->h) py = c->h - 1;
    // On the bezel: step toward the middle until the glass, so a drag
    // that leaves the tube keeps selecting to its edge.
    int on = 1;
    for (int i = 0; i < 64 && c->map[py * c->w + px] == UCRT_BEZEL; i++) {
        on = 0;
        px += px < c->w / 2 ? 1 : px > c->w / 2 ? -1 : 0;
        py += py < c->h / 2 ? 1 : py > c->h / 2 ? -1 : 0;
    }
    uint32_t m = c->map[py * c->w + px];
    if (m == UCRT_BEZEL) return 0;
    *sx = (int)((m >> 16) / FIX);
    *sy = (int)((m & 0xFFFF) / FIX);
    return on;
}

// --- buffers -----------------------------------------------------------

static int ensure(struct ucrt *c, int w, int h) {
    if (c->w == w && c->h == h && c->buf) {
        if (c->look.curve && c->map_curve != c->look.curve) return build_map(c);
        return 0;
    }
    ucrt_free(c);
    c->hw = (w + 1) / 2;
    c->hh = (h + 1) / 2;
    c->buf = malloc((size_t)w * h * 4);
    c->map = malloc((size_t)w * h * 4);
    c->half = malloc((size_t)c->hw * c->hh * 4);
    c->half2 = malloc((size_t)c->hw * c->hh * 4);
    if (!c->buf || !c->map || !c->half || !c->half2) { ucrt_free(c); return -1; }
    c->w = w;
    c->h = h;
    c->map_curve = -1;
    return c->look.curve ? build_map(c) : 0;
}

// --- glow --------------------------------------------------------------

// One box-blur pass of radius r along rows (step 1) or columns (step
// hw), from `in` to `out`, by running sums -- r costs nothing per pixel.
static void box(const uint32_t *in, uint32_t *out, int n, int lines, int step, int line_step, int r) {
    for (int l = 0; l < lines; l++) {
        const uint32_t *src = in + l * line_step;
        uint32_t *dst = out + l * line_step;
        int sr = 0, sg = 0, sb = 0;
        // A reciprocal, not a division per channel per pixel: the window
        // is 2r+1 wide everywhere (the ends repeat the edge pixel).
        uint32_t inv = (1u << 16) / (uint32_t)(2 * r + 1);
        for (int i = -r; i <= r; i++) {
            int j = i < 0 ? 0 : i >= n ? n - 1 : i;
            uint32_t p = src[j * step];
            sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
        }
        for (int i = 0; i < n; i++) {
            dst[i * step] = (((uint32_t)sr * inv >> 16) << 16) | (((uint32_t)sg * inv >> 16) << 8) |
                            ((uint32_t)sb * inv >> 16);
            int a = i - r < 0 ? 0 : i - r, b = i + r + 1 >= n ? n - 1 : i + r + 1;
            uint32_t pa = src[a * step], pb = src[b * step];
            sr += (int)((pb >> 16) & 0xFF) - (int)((pa >> 16) & 0xFF);
            sg += (int)((pb >> 8) & 0xFF) - (int)((pa >> 8) & 0xFF);
            sb += (int)(pb & 0xFF) - (int)(pa & 0xFF);
        }
    }
}

static void build_glow(struct ucrt *c) {
    int w = c->w, h = c->h, hw = c->hw, hh = c->hh;
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++) {
            int x0 = 2 * x, y0 = 2 * y, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
            uint32_t a = c->buf[y0 * w + x0], b = c->buf[y0 * w + x1];
            uint32_t d = c->buf[y1 * w + x0], e = c->buf[y1 * w + x1];
            uint32_t r = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF) + ((d >> 16) & 0xFF) + ((e >> 16) & 0xFF)) / 4;
            uint32_t g = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF) + ((d >> 8) & 0xFF) + ((e >> 8) & 0xFF)) / 4;
            uint32_t bl = ((a & 0xFF) + (b & 0xFF) + (d & 0xFF) + (e & 0xFF)) / 4;
            c->half[y * hw + x] = (r << 16) | (g << 8) | bl;
        }
    // Radius in half-resolution pixels, from the scanline pitch -- which
    // the app derives from its font, so the bloom scales with the text.
    int r = c->period / 2 + c->look.glow;
    for (int pass = 0; pass < 2; pass++) {
        box(c->half, c->half2, hw, hh, 1, hw, r);
        box(c->half2, c->half, hh, hw, hw, 1, r);
    }
}

// --- compose -------------------------------------------------------------

static inline uint32_t chan(uint32_t p, int sh) { return (p >> sh) & 0xFF; }

static uint32_t rnd(struct ucrt *c) {
    c->rng ^= c->rng << 13;
    c->rng ^= c->rng >> 17;
    c->rng ^= c->rng << 5;
    return c->rng;
}

// Separable falloff: 256 at the middle, down by `v` at the edge, as u^4.
static int vig_gain(int i, int n, int v) {
    float u = (2.0f * i + 1) / n - 1;
    float f = u * u;
    return 256 - (int)(v * f * f);
}

int ucrt_apply(struct ucrt *c, struct ugfx_surface *s, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w < 8 || h < 8 || !ucrt_look_on(&c->look)) return 0;
    return ucrt_apply_from(c, &s->pixels[y * s->w + x], s->w, s, x, y, w, h);
}

int ucrt_apply_from(struct ucrt *c, const uint32_t *src, int stride,
                    struct ugfx_surface *s, int x, int y, int w, int h) {
    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= (long)y * stride; h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return 0;
    int plain = w < 8 || h < 8 || !ucrt_look_on(&c->look) || w > MAP_MAX || h > MAP_MAX;
    if (plain || ensure(c, w, h) != 0) {
        for (int r = 0; r < h; r++)
            if (&s->pixels[(y + r) * s->w + x] != &src[(long)r * stride])
                memcpy(&s->pixels[(y + r) * s->w + x], &src[(long)r * stride], (size_t)w * 4);
        return plain ? 0 : -1;
    }
    if (c->period < 2) c->period = 2;
    c->frame++;

    for (int r = 0; r < h; r++) memcpy(&c->buf[r * w], &src[(long)r * stride], (size_t)w * 4);
    const struct ucrt_look *l = &c->look;
    if (l->glow) build_glow(c);

    // The tables: per column the vignette's gain, per row the scanline x
    // vignette x flicker gain. Rebuilt per frame because flicker moves
    // the row gain. Static, so ONE thread at a time may apply.
    static int col_gain[MAP_MAX + 1], row_gain[MAP_MAX + 1];
    int vig = VIG[l->vignette];
    for (int i = 0; i < w; i++) col_gain[i] = vig ? vig_gain(i, w, vig) : 256;
    int flick = l->flicker ? 256 - (int)(rnd(c) % 10) : 256;
    int scan = SCAN[l->scanlines];
    int lines = c->src_lines;
    for (int r = 0; r < h; r++) {
        int phase = r % c->period, g = 256;
        if (lines > 0) {
            // The row's source line, against the next one's and the one
            // after: the last pixel row of a line is its scanline, and a
            // line four or more rows tall shades the row above it too.
            long l0 = (long)r * lines / h, l1 = (long)(r + 1) * lines / h,
                 l2 = (long)(r + 2) * lines / h;
            if (l1 != l0) g -= scan;
            else if (h >= 4 * lines && l2 != l0) g -= scan / 2;
        } else if (phase == c->period - 1) g -= scan;
        else if (c->period >= 4 && phase == c->period - 2) g -= scan / 2;
        int v = vig ? vig_gain(r, h, vig) : 256;
        row_gain[r] = g * v / 256 * flick / 256;
    }
    int glow = GLOW[l->glow];
    for (int r = 0; r < h; r++) {
        uint32_t *row = &c->buf[r * w];
        const uint32_t *gl = l->glow ? &c->half[(r / 2) * c->hw] : 0;
        int rg = row_gain[r];
        // The slot mask is the aperture grille shifted half a triad on
        // alternate scanline periods -- staggered, as on a shadow-mask tube.
        int shift = l->mask == UCRT_MASK_SLOT && (r / c->period) % 2 ? 1 : 0;
        for (int i = 0; i < w; i++) {
            uint32_t p = row[i];
            int R = chan(p, 16), G = chan(p, 8), B = chan(p, 0);
            if (gl) {
                uint32_t q = gl[i / 2];
                int gr = (int)chan(q, 16) * glow >> 8, gg = (int)chan(q, 8) * glow >> 8,
                    gb = (int)chan(q, 0) * glow >> 8;
                R = R + gr - (R * gr >> 8);   // screen: 1-(1-a)(1-b)
                G = G + gg - (G * gg >> 8);
                B = B + gb - (B * gb >> 8);
            }
            int mr = 256, mg = 256, mb = 256;
            if (l->mask) {
                int t = (i + shift) % 3;
                mr = t == 0 ? 256 : 256 - MASK_DIM;
                mg = t == 1 ? 256 : 256 - MASK_DIM;
                mb = t == 2 ? 256 : 256 - MASK_DIM;
            }
            int cg = col_gain[i] * rg >> 8;
            R = R * mr >> 8; G = G * mg >> 8; B = B * mb >> 8;
            R = R * cg >> 8; G = G * cg >> 8; B = B * cg >> 8;
            if (l->noise) {
                int n = (int)(rnd(c) % 13) - 6;
                R += n; G += n; B += n;
            }
            R = R < 0 ? 0 : R > 255 ? 255 : R;
            G = G < 0 ? 0 : G > 255 ? 255 : G;
            B = B < 0 ? 0 : B > 255 ? 255 : B;
            row[i] = ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
        }
    }

    if (!l->curve) {
        for (int r = 0; r < h; r++) memcpy(&s->pixels[(y + r) * s->w + x], &c->buf[r * w], (size_t)w * 4);
        return 0;
    }
    for (int r = 0; r < h; r++) {
        uint32_t *out = &s->pixels[(y + r) * s->w + x];
        const uint32_t *m = &c->map[r * w];
        for (int i = 0; i < w; i++) {
            if (m[i] == UCRT_BEZEL) { out[i] = c->bezel; continue; }
            int fx = (int)(m[i] >> 16), fy = (int)(m[i] & 0xFFFF);
            int x0 = fx / FIX, y0 = fy / FIX, ax = fx % FIX, ay = fy % FIX;
            int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
            uint32_t p00 = c->buf[y0 * w + x0], p01 = c->buf[y0 * w + x1];
            uint32_t p10 = c->buf[y1 * w + x0], p11 = c->buf[y1 * w + x1];
            uint32_t v = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int top = (int)chan(p00, sh) * (FIX - ax) + (int)chan(p01, sh) * ax;
                int bot = (int)chan(p10, sh) * (FIX - ax) + (int)chan(p11, sh) * ax;
                v |= (uint32_t)((top * (FIX - ay) + bot * ay) / (FIX * FIX)) << sh;
            }
            out[i] = v;
        }
    }
    return 0;
}
