// See uambient.h. Moved out of the Image Viewer when the Audio Player
// became its second caller.
#include "ui/uambient.h"
#include "ui/utheme.h"
#include <stdlib.h>

void uambient_default(struct uambient *a) {
    a->centre = ugfx_rgb(52, 52, 58);
    a->edge   = ugfx_rgb(22, 22, 26);
    a->strip  = ugfx_rgb(38, 38, 43);
    a->chrome = UTHEME_CHROME;
    a->chrome_line = ugfx_blend(UTHEME_CHROME, ugfx_rgb(0, 0, 0), 34);
    a->panel  = ugfx_blend(UTHEME_CHROME, ugfx_rgb(255, 255, 255), 110);
}

static int lum(uint32_t c) {
    return (int)(((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF)) / 10;
}

// The picture's average, and the average of its darkest quarter, from a
// 16x9 sample.
void uambient_from(struct uambient *a, const struct uimg *im) {
    struct uimg s;
    if (!im || uimg_scale(im, 16, 9, &s) != 0) { uambient_default(a); return; }
    enum { N = 16 * 9 };
    uint32_t px[N];
    long r = 0, g = 0, b = 0;
    for (int i = 0; i < N; i++) {
        px[i] = s.px[i];
        r += (px[i] >> 16) & 0xFF; g += (px[i] >> 8) & 0xFF; b += px[i] & 0xFF;
    }
    uimg_free(&s);
    // The darkest quarter, by an insertion sort on luminance: 144 values.
    for (int i = 1; i < N; i++) {
        uint32_t v = px[i];
        int j = i - 1;
        while (j >= 0 && lum(px[j]) > lum(v)) { px[j + 1] = px[j]; j--; }
        px[j + 1] = v;
    }
    long dr = 0, dg = 0, db = 0;
    for (int i = 0; i < N / 4; i++) {
        dr += (px[i] >> 16) & 0xFF; dg += (px[i] >> 8) & 0xFF; db += px[i] & 0xFF;
    }
    uint32_t avg = ugfx_rgb((uint8_t)(r / N), (uint8_t)(g / N), (uint8_t)(b / N));
    uint32_t dark = ugfx_rgb((uint8_t)(dr * 4 / N), (uint8_t)(dg * 4 / N), (uint8_t)(db * 4 / N));
    uint32_t black = ugfx_rgb(0, 0, 0), white = ugfx_rgb(255, 255, 255);
    a->centre = ugfx_blend(avg, black, 40);
    a->edge   = ugfx_blend(dark, black, 140);
    a->strip  = ugfx_blend(avg, black, 170);
    // A FAINT wash on light chrome: dark text has to keep its contrast.
    a->chrome = ugfx_blend(UTHEME_CHROME, avg, 26);
    a->chrome_line = ugfx_blend(a->chrome, black, 34);
    a->panel  = ugfx_blend(a->chrome, white, 110);
}

void uambient_paint(struct uambient_stage *st, const struct uambient *a,
                    struct ugfx_surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!st->grad || st->w != w || st->h != h || st->c != a->centre || st->e != a->edge) {
        free(st->grad);
        st->grad = malloc((size_t)w * (size_t)h * sizeof *st->grad);
        if (!st->grad) { ugfx_fill_rect(s, x, y, w, h, a->edge); return; }
        st->w = w; st->h = h; st->c = a->centre; st->e = a->edge;
        // An ellipse a little wider than the stage, centred just above
        // the middle; t squared needs no square root and falls off as a
        // lit-from-behind ground should.
        int cx = w / 2, cy = h * 45 / 100;
        long rx = (long)w * 65 / 100 + 1, ry = (long)h * 75 / 100 + 1;
        for (int yy = 0; yy < h; yy++) {
            long dy = (long)(yy - cy) * 256 / ry;
            for (int xx = 0; xx < w; xx++) {
                long dx = (long)(xx - cx) * 256 / rx;
                long t = (dx * dx + dy * dy) >> 8;
                if (t > 255) t = 255;
                st->grad[(size_t)yy * w + xx] = ugfx_blend(st->c, st->e, (uint8_t)t);
            }
        }
    }
    ugfx_blit(s, x, y, w, h, st->grad, w);
}

void uambient_stage_free(struct uambient_stage *st) {
    free(st->grad);
    st->grad = 0;
}
