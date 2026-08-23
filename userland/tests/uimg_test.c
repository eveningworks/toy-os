// The JPEG decoder, in the ring where it actually runs.
//
// WHAT THIS ADDS OVER tools/uimg_hostcheck.py, which checks the same .c
// file against libjpeg over a couple of hundred images: NOTHING about
// the algorithm, and everything about toy-os. The host harness compiles
// uimg_jpeg.c with the host gcc, the host malloc and the host's idea of
// a `long`; this one runs it in ring 3, on this heap, in a real process
// -- the same gap /tests/klineedit_test and /tests/ttf_test exist for.
// A decoder that is perfect on the host and allocates past what
// SYS_SBRK gives it here fails only in this test.
//
// THE REFERENCE PIXELS ARE LIBJPEG'S, not this decoder's. They come
// from tools/gen_imgdata.py, which encodes each image with Pillow and
// records what Pillow decodes it back to. That is the whole point:
// comparing a decoder against its own output proves it is
// self-consistent, which a decoder with a wrong IDCT constant also is.
//
// The tolerance is 3 per channel and that is not slack: two conforming
// IDCT implementations are allowed to differ (the JPEG spec specifies
// the transform, not an arithmetic), and libjpeg's islow and the
// Loeffler factorisation here disagree by a rounding step. Measured on
// the host: half the samples land exactly, and nothing exceeds 3.
// A one-unit error in a single IDCT constant is still caught, though
// only barely -- which is why the REFUSAL checks below matter too, and
// why hostcheck's breadth is the primary evidence.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "lib/uimg.h"
#include <kerrno.h>
#include "uimg_vectors.h"

#define TOLERANCE 3

static int g_fail;

static void put(const char *s) { sys_write(1, s, strlen(s)); }

static void ok(const char *name, int cond, const char *detail) {
    char line[192];
    snprintf(line, sizeof line, "  %s  %s%s%s\n", cond ? "ok  " : "FAIL",
             name, (!cond && detail) ? "  -- " : "", (!cond && detail) ? detail : "");
    put(line);
    if (!cond) g_fail++;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    put("uimg_test: the JPEG decoder, against libjpeg's own output\n");

    for (int i = 0; i < UIMG_VECTOR_COUNT; i++) {
        const struct uimg_vector *v = &uimg_vectors[i];
        char detail[160];

        if (v->err) {
            // A REFUSAL IS A RESULT. -ENOTSUP for a progressive JPEG and
            // -EINVAL for a broken one are different answers, and a
            // decoder that returned either for both would pass a test
            // that only asked "did it fail".
            struct uimg im;
            int rc = uimg_decode(v->jpeg, v->jpeg_len, &im);
            snprintf(detail, sizeof detail, "wanted %d, got %d (%s)",
                     -v->err, rc, uimg_last_error());
            ok(v->name, rc == -v->err, detail);
            if (rc == 0) uimg_free(&im);
            continue;
        }

        struct uimg_info info;
        int rc = uimg_info(v->jpeg, v->jpeg_len, &info);
        if (rc < 0) {
            snprintf(detail, sizeof detail, "info failed: %d (%s)", rc,
                     uimg_last_error());
            ok(v->name, 0, detail);
            continue;
        }
        if (info.w != v->w || info.h != v->h) {
            snprintf(detail, sizeof detail, "header says %dx%d, want %dx%d",
                     info.w, info.h, v->w, v->h);
            ok(v->name, 0, detail);
            continue;
        }

        struct uimg im;
        rc = uimg_decode(v->jpeg, v->jpeg_len, &im);
        if (rc < 0) {
            snprintf(detail, sizeof detail, "decode failed: %d (%s)", rc,
                     uimg_last_error());
            ok(v->name, 0, detail);
            continue;
        }

        int worst = 0, worst_at = -1;
        long total = 0;
        for (int p = 0; p < im.w * im.h; p++) {
            uint32_t got = im.px[p];
            const unsigned char *want = v->rgb + (size_t)p * 3;
            int ch[3] = { (int)((got >> 16) & 0xFF), (int)((got >> 8) & 0xFF),
                          (int)(got & 0xFF) };
            for (int k = 0; k < 3; k++) {
                int d = ch[k] - (int)want[k];
                if (d < 0) d = -d;
                total += d;
                if (d > worst) { worst = d; worst_at = p; }
            }
        }
        snprintf(detail, sizeof detail,
                 "worst channel off by %d at pixel %d,%d; mean %ld/1000",
                 worst, worst_at % im.w, worst_at / im.w,
                 (total * 1000) / (im.w * im.h * 3));
        ok(v->name, worst <= TOLERANCE, detail);
        uimg_free(&im);
    }

    // --- the resampler ---------------------------------------------
    //
    // Scaling is checked on a SYNTHETIC image rather than a decoded one,
    // because a decoded one cannot say what the answer should be. A
    // 4x4 image of one colour must scale to that colour at any size --
    // which catches an off-by-one in the weights that a photograph
    // would merely blur.
    struct uimg flat;
    flat.w = flat.h = 4;
    static uint32_t flatpx[16];
    for (int i = 0; i < 16; i++) flatpx[i] = 0x336699;
    flat.px = flatpx;

    struct uimg up, down;
    int rc = uimg_scale(&flat, 37, 11, &up);
    int flat_ok = (rc == 0);
    if (flat_ok) {
        for (int i = 0; i < up.w * up.h; i++)
            if (up.px[i] != 0x336699) { flat_ok = 0; break; }
    }
    ok("a flat image scales up to the same flat colour", flat_ok, "a weight is wrong");
    if (rc == 0) uimg_free(&up);

    rc = uimg_scale(&flat, 2, 2, &down);
    int down_ok = (rc == 0 && down.w == 2 && down.h == 2);
    if (down_ok)
        for (int i = 0; i < 4; i++)
            if (down.px[i] != 0x336699) { down_ok = 0; break; }
    ok("and down to the same flat colour", down_ok, "a weight is wrong");
    if (rc == 0) uimg_free(&down);

    // A two-tone image AVERAGED down must land between its two colours,
    // which nearest-neighbour cannot do -- this is the check that tells
    // a box filter from a pixel-picker.
    static uint32_t two[4];
    two[0] = two[1] = 0x000000;
    two[2] = two[3] = 0xFFFFFF;
    struct uimg tt = { .w = 2, .h = 2, .px = two }, half;
    rc = uimg_scale(&tt, 1, 1, &half);
    int mid = (rc == 0) ? (int)(half.px[0] & 0xFF) : -1;
    char d2[64];
    snprintf(d2, sizeof d2, "got %d, wanted about 127", mid);
    ok("averaging two black and two white pixels gives grey",
       mid >= 120 && mid <= 135, d2);
    if (rc == 0) uimg_free(&half);

    // fit maths, which the widget and the wallpaper both depend on
    int fw, fh;
    uimg_fit_size(200, 100, 50, 50, UIMG_FIT_CONTAIN, &fw, &fh);
    ok("contain fits the wide axis", fw == 50 && fh == 25, "wrong contain size");
    uimg_fit_size(200, 100, 50, 50, UIMG_FIT_COVER, &fw, &fh);
    ok("cover fills the short axis", fw == 100 && fh == 50, "wrong cover size");

    char tail[64];
    snprintf(tail, sizeof tail, "uimg_test: %d failure(s)\n", g_fail);
    put(tail);
    if (g_fail == 0) put("uimg_test: all checks passed\n");
    return g_fail;
}
