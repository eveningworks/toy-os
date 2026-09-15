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
// EACH VECTOR CARRIES ITS OWN TOLERANCE, and the two values are not a
// judgement call. JPEG's is 3, because two conforming IDCTs are allowed
// to differ (the spec fixes the transform, not an arithmetic) and
// libjpeg's islow disagrees with the Loeffler factorisation here by a
// rounding step. QOI's is 0: it is lossless, so "close enough" is not a
// thing that exists, and a single wrong pixel is a bug.
//
// A one-unit error in a single IDCT constant is caught only barely at 3
// -- which is why the REFUSAL checks below matter too, and why
// hostcheck's breadth is the primary evidence for the JPEG path. The
// QOI path needs no such hedging.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "lib/uimg.h"
#include <kerrno.h>
#include "uimg_vectors.h"

#include "lib/utest.h"

// The call sites here read `ok(name, cond, detail)`; the harness takes
// the boolean first. One adapter rather than transposing every call
// site: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void ok(const char *name, int cond, const char *detail) {
    utest_check_detail(cond, name, detail);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    utest_begin("uimg_test", "the image decoders, against Pillow's own output", 0);

    for (int i = 0; i < UIMG_VECTOR_COUNT; i++) {
        const struct uimg_vector *v = &uimg_vectors[i];
        char detail[160];

        if (v->err) {
            // A REFUSAL IS A RESULT. -ENOTSUP for a progressive JPEG and
            // -EINVAL for a broken one are different answers, and a
            // decoder that returned either for both would pass a test
            // that only asked "did it fail".
            struct uimg im;
            int rc = uimg_decode(v->data, v->len, &im);
            snprintf(detail, sizeof detail, "wanted %d, got %d (%s)",
                     -v->err, rc, uimg_last_error());
            ok(v->name, rc == -v->err, detail);
            if (rc == 0) uimg_free(&im);
            continue;
        }

        struct uimg_info info;
        int rc = uimg_info(v->data, v->len, &info);
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
        rc = uimg_decode(v->data, v->len, &im);
        if (rc < 0) {
            snprintf(detail, sizeof detail, "decode failed: %d (%s)", rc,
                     uimg_last_error());
            ok(v->name, 0, detail);
            continue;
        }

        // ALL FOUR CHANNELS, and alpha EXACTLY whatever the tolerance
        // is: a lossy codec may disagree about colour, never about
        // whether a pixel is opaque. The JPEG vectors carry alpha 255
        // throughout, so this is also what proves the JPEG path fills
        // the byte rather than leaving it zero.
        int worst = 0, worst_at = -1, alpha_bad = 0;
        long total = 0;
        for (int p = 0; p < im.w * im.h; p++) {
            uint32_t got = im.px[p];
            const unsigned char *want = v->rgba + (size_t)p * 4;
            int ch[4] = { (int)((got >> 16) & 0xFF), (int)((got >> 8) & 0xFF),
                          (int)(got & 0xFF), (int)((got >> 24) & 0xFF) };
            const int wa[4] = { want[0], want[1], want[2], want[3] };
            for (int k = 0; k < 3; k++) {
                int d = ch[k] - wa[k];
                if (d < 0) d = -d;
                total += d;
                if (d > worst) { worst = d; worst_at = p; }
            }
            if (ch[3] != wa[3]) alpha_bad++;
        }
        snprintf(detail, sizeof detail,
                 "worst channel off by %d (tol %d) at pixel %d,%d; mean %ld/1000; "
                 "%d wrong alpha",
                 worst, v->tol, worst_at % im.w, worst_at / im.w,
                 (total * 1000) / (im.w * im.h * 3), alpha_bad);
        ok(v->name, worst <= v->tol && alpha_bad == 0, detail);
        uimg_free(&im);
    }

    // --- the resampler ---------------------------------------------
    //
    // Scaling is checked on a SYNTHETIC image rather than a decoded one,
    // because a decoded one cannot say what the answer should be. A
    // 4x4 image of one colour must scale to that colour at any size --
    // which catches an off-by-one in the weights that a photograph
    // would merely blur.
    // OPAQUE, and the alpha byte is the point: since the resampler
    // weights colour BY alpha (uimg.c says why), a fixture left at
    // alpha 0 scales to transparent black and every check below fails
    // for a reason that has nothing to do with the weights. This
    // fixture said 0x336699 until the alpha channel landed, and that is
    // exactly how it failed.
    struct uimg flat;
    flat.w = flat.h = 4;
    flat.has_alpha = 0;
    static uint32_t flatpx[16];
    for (int i = 0; i < 16; i++) flatpx[i] = 0xFF336699;
    flat.px = flatpx;

    struct uimg up, down;
    int rc = uimg_scale(&flat, 37, 11, &up);
    int flat_ok = (rc == 0);
    if (flat_ok) {
        for (int i = 0; i < up.w * up.h; i++)
            if (up.px[i] != 0xFF336699) { flat_ok = 0; break; }
    }
    ok("a flat image scales up to the same flat colour", flat_ok, "a weight is wrong");
    if (rc == 0) uimg_free(&up);

    rc = uimg_scale(&flat, 2, 2, &down);
    int down_ok = (rc == 0 && down.w == 2 && down.h == 2);
    if (down_ok)
        for (int i = 0; i < 4; i++)
            if (down.px[i] != 0xFF336699) { down_ok = 0; break; }
    ok("and down to the same flat colour", down_ok, "a weight is wrong");
    if (rc == 0) uimg_free(&down);

    // A two-tone image AVERAGED down must land between its two colours,
    // which nearest-neighbour cannot do -- this is the check that tells
    // a box filter from a pixel-picker.
    static uint32_t two[4];
    two[0] = two[1] = 0xFF000000;
    two[2] = two[3] = 0xFFFFFFFF;
    struct uimg tt = { .w = 2, .h = 2, .px = two, .has_alpha = 0 }, half;
    rc = uimg_scale(&tt, 1, 1, &half);
    int mid = (rc == 0) ? (int)(half.px[0] & 0xFF) : -1;
    char d2[64];
    snprintf(d2, sizeof d2, "got %d, wanted about 127", mid);
    ok("averaging two black and two white pixels gives grey",
       mid >= 120 && mid <= 135, d2);
    if (rc == 0) uimg_free(&half);

    // --- and the check the alpha channel exists for -------------------
    //
    // Two OPAQUE RED pixels and two FULLY TRANSPARENT ones, averaged
    // into one. The alpha must land near half -- and the colour must
    // stay PURE RED.
    //
    // That second half is the whole point. Averaging straight RGBA
    // channel by channel gives (128, 0, 0) at alpha 128, because the
    // transparent pixels contribute their colour (which a generator
    // writes as black) as if it were visible. That is the dark halo
    // every naively downscaled icon has, and it is invisible in any
    // test that only checks alpha.
    static uint32_t halo[4];
    halo[0] = halo[1] = 0xFFFF0000;   // opaque red
    halo[2] = halo[3] = 0x00000000;   // transparent black
    struct uimg ha = { .w = 2, .h = 2, .px = halo, .has_alpha = 1 }, hb;
    rc = uimg_scale(&ha, 1, 1, &hb);
    uint32_t hp = (rc == 0) ? hb.px[0] : 0;
    int ha_a = (int)((hp >> 24) & 0xFF), ha_r = (int)((hp >> 16) & 0xFF);
    int ha_g = (int)((hp >> 8) & 0xFF), ha_b = (int)(hp & 0xFF);
    char d3[96];
    snprintf(d3, sizeof d3, "got a=%d r=%d g=%d b=%d, wanted a~128 r=255 g=0 b=0",
             ha_a, ha_r, ha_g, ha_b);
    ok("a transparent pixel does not bleed its colour into an opaque one",
       ha_a >= 120 && ha_a <= 136 && ha_r >= 250 && ha_g == 0 && ha_b == 0, d3);
    if (rc == 0) uimg_free(&hb);

    // --- the ENCODERS, round-tripped -----------------------------------
    //
    // **THE ROUND TRIP IS THE WEAK HALF AND IT IS HERE ANYWAY.** An
    // encoder checked by this repo's own decoder passes whenever the two
    // share a mistake, which is why the real oracle is Pillow and zlib
    // on the host (tools/uimg_encode_hostcheck.py). What this adds is
    // the half that harness cannot see: the encoders running in RING 3,
    // against this heap and these syscalls, on the machine that ships.
    //
    // The image is deliberately awkward for QOI: a run, an index hit, a
    // small delta and a jump too big for one, so every chunk type is
    // exercised rather than a flat block that only emits runs.
    static uint32_t src[16];
    for (int i = 0; i < 16; i++) src[i] = 0xFF000000u | (uint32_t)(i * 0x0F0704);
    src[4] = src[5] = src[6] = src[3];        // a run
    src[9] = src[0];                          // an index hit
    src[12] = 0xFF010203u;                    // a jump nothing can encode small
    struct uimg enc = { .w = 4, .h = 4, .px = src, .has_alpha = 0 };

    uint8_t *bytes = 0;
    size_t len = 0;
    rc = uimg_encode(&enc, "qoi", &bytes, &len);
    ok("QOI encodes", rc == 0 && bytes && len > 14, uimg_last_error());
    if (rc == 0) {
        struct uimg back;
        int drc = uimg_decode(bytes, len, &back);
        int same = drc == 0 && back.w == 4 && back.h == 4;
        for (int i = 0; same && i < 16; i++)
            if ((back.px[i] & 0xFFFFFF) != (src[i] & 0xFFFFFF)) same = 0;
        ok("QOI decodes back to the same pixels, exactly", same,
           drc == 0 ? "a pixel differs" : uimg_last_error());
        if (drc == 0) uimg_free(&back);
        free(bytes);
    }

    // PNG has no decoder here, and that is a DISTINCT answer from a
    // broken file -- an app has to be able to tell a user which it is.
    bytes = 0;
    len = 0;
    rc = uimg_encode(&enc, "png", &bytes, &len);
    ok("PNG encodes", rc == 0 && bytes && len > 8, uimg_last_error());
    if (rc == 0) {
        struct uimg_info info;
        int irc = uimg_info(bytes, len, &info);
        char d4[96];
        snprintf(d4, sizeof d4, "info rc=%d %dx%d %s", irc, info.w, info.h,
                 info.format ? info.format : "?");
        ok("a written PNG reads back its own header",
           irc == 0 && info.w == 4 && info.h == 4, d4);

        struct uimg back;
        int drc = uimg_decode(bytes, len, &back);
        ok("and refuses to DECODE with -ENOTSUP, not -EINVAL",
           drc == -ENOTSUP, "a file we wrote must not read as corrupt");
        free(bytes);
    }

    rc = uimg_encode(&enc, "jpeg", &bytes, &len);
    ok("a format with no encoder is -ENOTSUP", rc == -ENOTSUP, uimg_last_error());

    // fit maths, which the widget and the wallpaper both depend on
    int fw, fh;
    uimg_fit_size(200, 100, 50, 50, UIMG_FIT_CONTAIN, &fw, &fh);
    ok("contain fits the wide axis", fw == 50 && fh == 25, "wrong contain size");
    uimg_fit_size(200, 100, 50, 50, UIMG_FIT_COVER, &fw, &fh);
    ok("cover fills the short axis", fw == 100 && fh == 50, "wrong cover size");

    return utest_end();
}
