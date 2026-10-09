// Drawing a uvid frame: colour conversion and scaling in ONE pass, at
// the destination's size (lib/uvid.h, uvid_frame_draw).
//
// **SEPARABLE, WITH TWO CACHED ROWS.** Bilinear is a horizontal blend
// then a vertical one; the horizontal pass is done once per SOURCE row
// into a row cache and the vertical one per destination pixel, so an
// upscale reads each source row once however many screen rows it
// covers. The colour conversion runs on the blended Y/Cb/Cr, at the
// destination's size -- what a GPU's sampler and a video overlay plane
// do in hardware, done here on the CPU.
//
// BT.601 coefficients in 16.16 tables, one set for MPEG's studio range
// (16..235) and one for JFIF's full range. Chroma samples sit BETWEEN
// the luma samples (MPEG-1's and JFIF's siting), so a chroma position is
// half the luma one, less a quarter.
#include "lib/uvid.h"
#include "lib/uimg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int32_t g_t[2][5][256];  // [full][y, r<-cr, g<-cb, g<-cr, b<-cb]
static int g_ready;

static void build_tables(void) {
    if (g_ready) return;
    for (int v = 0; v < 256; v++) {
        int c = v - 128;
        g_t[0][0][v] = (v - 16) * 76309;    // 1.164383
        g_t[0][1][v] = c * 104597;          // 1.596027
        g_t[0][2][v] = c * 25675;           // 0.391762
        g_t[0][3][v] = c * 53279;           // 0.812968
        g_t[0][4][v] = c * 132201;          // 2.017232
        g_t[1][0][v] = v << 16;
        g_t[1][1][v] = c * 91881;           // 1.402
        g_t[1][2][v] = c * 22554;           // 0.344136
        g_t[1][3][v] = c * 46802;           // 0.714136
        g_t[1][4][v] = c * 116130;          // 1.772
    }
    g_ready = 1;
}

static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline uint32_t yuv_px(const int32_t (*t)[256], int y, int cb, int cr) {
    int32_t Y = t[0][y] + 32768;
    int r = (Y + t[1][cr]) >> 16;
    int g = (Y - t[2][cb] - t[3][cr]) >> 16;
    int b = (Y + t[4][cb]) >> 16;
    return (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | (uint32_t)clamp8(b);
}

// The 16.16 source position of destination sample `x`'s centre, for a
// source span starting at `s0` (16.16) and `sw` (16.16) wide.
static inline int32_t src_pos(int x, int64_t s0, int64_t sw, int dw) {
    return (int32_t)(s0 + ((2 * (int64_t)x + 1) * sw) / (2 * dw) - 32768);
}

static inline int32_t clamp_pos(int32_t p, int n) {
    if (p < 0) return 0;
    int32_t hi = (int32_t)(n - 1) << 16;
    return p > hi ? hi : p;
}

// --- the row cache -----------------------------------------------------

struct rows {
    int idx[2];
    uint8_t *b[2];
};

// Which of the two slots holds source row `want`, filling one if
// neither does -- never the one holding row `spare`, the other row the
// caller needs for this destination row.
static int row_slot(struct rows *r, int want, int spare,
                    const uint8_t *plane, int stride, const int *xi, const uint8_t *xf, int dw, int pw) {
    if (r->idx[0] == want) return 0;
    if (r->idx[1] == want) return 1;
    int s = r->idx[0] == spare ? 1 : 0;
    const uint8_t *src = plane + (size_t)want * (size_t)stride;
    uint8_t *out = r->b[s];
    for (int x = 0; x < dw; x++) {
        int i = xi[x], f = xf[x];
        int j = i + 1 < pw ? i + 1 : i;
        out[x] = (uint8_t)((src[i] * (256 - f) + src[j] * f + 128) >> 8);
    }
    r->idx[s] = want;
    return s;
}

static void htab(int *xi, uint8_t *xf, int dw, int64_t s0, int64_t sw, int n, int chroma) {
    for (int x = 0; x < dw; x++) {
        int32_t p = src_pos(x, s0, sw, dw);
        if (chroma) p = p / 2 - 16384;
        p = clamp_pos(p, n);
        xi[x] = p >> 16;
        xf[x] = (uint8_t)((p >> 8) & 255);
    }
}

static void draw_yuv_smooth(const struct uvid_frame *f, int sx, int sy, int sw, int sh,
                            uint32_t *dst, int ds, int dw, int dh) {
    const int32_t (*t)[256] = g_t[f->full_range ? 1 : 0];
    int cw = (f->w + 1) / 2, chh = (f->h + 1) / 2;
    size_t need = (size_t)dw * (2 * sizeof(int) + 2 + 6);
    uint8_t *mem = malloc(need);
    if (!mem) return;
    int *xi = (int *)mem, *cxi = xi + dw;
    uint8_t *xf = (uint8_t *)(cxi + dw), *cxf = xf + dw, *rb = cxf + dw;
    struct rows ry = { { -1, -1 }, { rb, rb + dw } };
    struct rows ru = { { -1, -1 }, { rb + 2 * dw, rb + 3 * dw } };
    struct rows rv = { { -1, -1 }, { rb + 4 * dw, rb + 5 * dw } };

    int64_t s0x = (int64_t)sx << 16, swx = (int64_t)sw << 16;
    int64_t s0y = (int64_t)sy << 16, swy = (int64_t)sh << 16;
    htab(xi, xf, dw, s0x, swx, f->w, 0);
    htab(cxi, cxf, dw, s0x, swx, cw, 1);

    for (int y = 0; y < dh; y++) {
        int32_t p = src_pos(y, s0y, swy, dh);
        int32_t lp = clamp_pos(p, f->h), cp = clamp_pos(p / 2 - 16384, chh);
        int ly = lp >> 16, lf = (lp >> 8) & 255, ly1 = ly + 1 < f->h ? ly + 1 : ly;
        int cy = cp >> 16, cf = (cp >> 8) & 255, cy1 = cy + 1 < chh ? cy + 1 : cy;

        int a = row_slot(&ry, ly, ly1, f->y, f->y_stride, xi, xf, dw, f->w);
        int b = row_slot(&ry, ly1, ly, f->y, f->y_stride, xi, xf, dw, f->w);
        int ua = row_slot(&ru, cy, cy1, f->cb, f->c_stride, cxi, cxf, dw, cw);
        int ub = row_slot(&ru, cy1, cy, f->cb, f->c_stride, cxi, cxf, dw, cw);
        int va = row_slot(&rv, cy, cy1, f->cr, f->c_stride, cxi, cxf, dw, cw);
        int vb = row_slot(&rv, cy1, cy, f->cr, f->c_stride, cxi, cxf, dw, cw);
        const uint8_t *Y0 = ry.b[a], *Y1 = ry.b[b];
        const uint8_t *U0 = ru.b[ua], *U1 = ru.b[ub], *V0 = rv.b[va], *V1 = rv.b[vb];
        uint32_t *out = dst + (size_t)y * (size_t)ds;
        int lf1 = 256 - lf, cf1 = 256 - cf;
        for (int x = 0; x < dw; x++) {
            int Yv = (Y0[x] * lf1 + Y1[x] * lf + 128) >> 8;
            int Uv = (U0[x] * cf1 + U1[x] * cf + 128) >> 8;
            int Vv = (V0[x] * cf1 + V1[x] * cf + 128) >> 8;
            out[x] = yuv_px(t, Yv, Uv, Vv);
        }
    }
    free(mem);
}

static void draw_yuv_fast(const struct uvid_frame *f, int sx, int sy, int sw, int sh,
                          uint32_t *dst, int ds, int dw, int dh) {
    const int32_t (*t)[256] = g_t[f->full_range ? 1 : 0];
    int *xi = malloc((size_t)dw * sizeof *xi);
    if (!xi) return;
    for (int x = 0; x < dw; x++)
        xi[x] = clamp_pos(src_pos(x, (int64_t)sx << 16, (int64_t)sw << 16, dw) + 32768, f->w) >> 16;
    for (int y = 0; y < dh; y++) {
        int ly = clamp_pos(src_pos(y, (int64_t)sy << 16, (int64_t)sh << 16, dh) + 32768, f->h) >> 16;
        const uint8_t *Y = f->y + (size_t)ly * (size_t)f->y_stride;
        const uint8_t *U = f->cb + (size_t)(ly / 2) * (size_t)f->c_stride;
        const uint8_t *V = f->cr + (size_t)(ly / 2) * (size_t)f->c_stride;
        uint32_t *out = dst + (size_t)y * (size_t)ds;
        for (int x = 0; x < dw; x++) {
            int i = xi[x];
            out[x] = yuv_px(t, Y[i], U[i / 2], V[i / 2]);
        }
    }
    free(xi);
}

// --- packed RGB ------------------------------------------------------

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t f) {
    uint32_t g = 256 - f;
    uint32_t rb = (((a & 0xff00ff) * g + (b & 0xff00ff) * f) >> 8) & 0xff00ff;
    uint32_t gg = (((a & 0x00ff00) * g + (b & 0x00ff00) * f) >> 8) & 0x00ff00;
    return rb | gg;
}

static void draw_argb(const struct uvid_frame *f, int sx, int sy, int sw, int sh,
                      uint32_t *dst, int ds, int dw, int dh, int fast) {
    int *xi = malloc((size_t)dw * (sizeof(int) + 1) + (size_t)dw * 2 * sizeof(uint32_t));
    if (!xi) return;
    uint32_t *r0 = (uint32_t *)(xi + dw), *r1 = r0 + dw;
    uint8_t *xf = (uint8_t *)(r1 + dw);
    int64_t s0x = (int64_t)sx << 16, swx = (int64_t)sw << 16;
    for (int x = 0; x < dw; x++) {
        int32_t p = clamp_pos(src_pos(x, s0x, swx, dw) + (fast ? 32768 : 0), f->w);
        xi[x] = p >> 16;
        xf[x] = fast ? 0 : (uint8_t)((p >> 8) & 255);
    }
    int idx0 = -1, idx1 = -1;
    for (int y = 0; y < dh; y++) {
        int32_t p = clamp_pos(src_pos(y, (int64_t)sy << 16, (int64_t)sh << 16, dh) + (fast ? 32768 : 0), f->h);
        int ly = p >> 16, lf = fast ? 0 : (p >> 8) & 255;
        int ly1 = ly + 1 < f->h ? ly + 1 : ly;
        // Rows are filled in order, so the pair only ever slides down.
        if (idx1 == ly) { uint32_t *t = r0; r0 = r1; r1 = t; idx0 = idx1; idx1 = -1; }
        for (int k = 0; k < 2; k++) {
            int want = k ? ly1 : ly;
            int *have = k ? &idx1 : &idx0;
            if (*have == want) continue;
            const uint32_t *src = f->argb + (size_t)want * (size_t)f->argb_stride;
            uint32_t *o = k ? r1 : r0;
            for (int x = 0; x < dw; x++) {
                int i = xi[x], j = i + 1 < f->w ? i + 1 : i;
                o[x] = xf[x] ? lerp_px(src[i], src[j], xf[x]) : src[i] & 0xffffff;
            }
            *have = want;
        }
        uint32_t *out = dst + (size_t)y * (size_t)ds;
        if (!lf) memcpy(out, r0, (size_t)dw * sizeof *out);
        else for (int x = 0; x < dw; x++) out[x] = lerp_px(r0[x], r1[x], (uint32_t)lf);
    }
    free(xi);
}

void uvid_frame_draw(const struct uvid_frame *f, int sx, int sy, int sw, int sh,
                     uint32_t *dst, int dst_stride, int dw, int dh, int quality) {
    if (!f || dw <= 0 || dh <= 0 || f->w <= 0 || f->h <= 0) return;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > f->w) sw = f->w - sx;
    if (sy + sh > f->h) sh = f->h - sy;
    if (sw <= 0 || sh <= 0) return;
    build_tables();
    if (f->fmt == UVID_ARGB)
        draw_argb(f, sx, sy, sw, sh, dst, dst_stride, dw, dh, quality == UVID_FAST);
    else if (quality == UVID_FAST)
        draw_yuv_fast(f, sx, sy, sw, sh, dst, dst_stride, dw, dh);
    else
        draw_yuv_smooth(f, sx, sy, sw, sh, dst, dst_stride, dw, dh);
}

int uvid_frame_to_uimg(const struct uvid_frame *f, int max_w, int max_h, struct uimg *out) {
    memset(out, 0, sizeof *out);
    int w = f->w, h = f->h;
    if (max_w > 0 && max_h > 0 && (w > max_w || h > max_h)) {
        // The smaller of the two ratios, so the whole picture fits.
        if ((int64_t)w * max_h > (int64_t)h * max_w) {
            h = (int)((int64_t)h * max_w / w);
            w = max_w;
        } else {
            w = (int)((int64_t)w * max_h / h);
            h = max_h;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    out->px = malloc((size_t)w * (size_t)h * sizeof *out->px);
    if (!out->px) return -ENOMEM;
    out->w = w;
    out->h = h;
    uvid_frame_draw(f, 0, 0, f->w, f->h, out->px, w, w, h, UVID_SMOOTH);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) out->px[i] |= 0xff000000u;
    return 0;
}
