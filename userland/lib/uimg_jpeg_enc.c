// Writing a baseline JPEG, in ring 3.
//
// The decoder next door (uimg_jpeg.c) is the interesting half; this one
// is deliberately the plain textbook encoder, because a JPEG's value is
// that everything opens it and cleverness here buys compression at the
// cost of that. So: baseline sequential, Huffman, 4:2:0, the
// quantisation and Huffman tables out of the standard's own Annex K.
//
// WHAT IT DOES NOT DO, and why none is a gap worth closing yet:
//   * OPTIMISED HUFFMAN TABLES. Two passes over the image to build a
//     code table fitted to it, for ~5% off the file. libjpeg makes it
//     an option and leaves it off by default.
//   * PROGRESSIVE OUTPUT. The decoder reads it now; nothing here has a
//     reason to write it, since nothing serves images over a link.
//   * A QUALITY KNOB IN uimg_encode(), which takes a format and no
//     options -- QOI and PNG take none either. uimg_encode_jpeg() takes
//     the one number, for remoted's Tight; J_ENC_QUALITY is the default.
//   * 4:4:4 or grayscale output. 4:2:0 halves the chroma and is what
//     every camera and every export dialog produces.
//
// NO FLOATING POINT, as everywhere here. The forward DCT is the IJG's
// `jpeg_fdct_islow` integer factorisation -- 13-bit constants, two
// passes -- which is the exact inverse in arrangement of the decoder's
// Loeffler IDCT and the reason our output decodes to what we put in.
#include "lib/uimg.h"
#include "lib/uimg_jpeg_int.h"
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(code, msg) do { uimg_set_error(msg); return (code); } while (0)

// libjpeg's default is 75 and every export dialog's is 85-92. 85 is the
// point where a photograph stops showing ringing on a hard edge at
// normal viewing size, and it is what GIMP, ImageMagick and macOS all
// land on.
#define J_ENC_QUALITY 85

// --- the standard tables (ITU T.81 Annex K) ---------------------------

static const uint8_t j_qt_lum[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99,
};
static const uint8_t j_qt_chr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

// A symbol's canonical code, derived from the bit-length counts exactly
// as the decoder derives its own -- the two are the same construction
// read in opposite directions, which is what makes a file this writes
// one that file's own decoder agrees with.
struct jenc_huff {
    uint16_t code[256];
    uint8_t  len[256];
};

static void j_build_enc_huff(struct jenc_huff *h, const uint8_t *bits,
                             const uint8_t *vals) {
    memset(h, 0, sizeof *h);
    unsigned code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l]; i++) {
            h->code[vals[k]] = (uint16_t)code;
            h->len[vals[k]] = (uint8_t)l;
            k++;
            code++;
        }
        code <<= 1;
    }
}

// --- the output buffer ------------------------------------------------
//
// GROWN, not sized in advance: a JPEG's length is not known until it is
// written, and the honest upper bound (bigger than the raw pixels) is
// an allocation nobody wants. Doubling, starting at a guess.
struct jenc {
    uint8_t *buf;
    size_t len, cap;
    int oom;

    uint32_t bitbuf;
    int bitcnt;

    struct jenc_huff dc[2], ac[2];
    uint16_t qt[2][64];    // NATURAL order, ready for the quantise loop
};

static void j_put(struct jenc *e, uint8_t b) {
    if (e->oom) return;
    if (e->len == e->cap) {
        size_t want = e->cap ? e->cap * 2 : 65536;
        uint8_t *p = realloc(e->buf, want);
        if (!p) { e->oom = 1; return; }
        e->buf = p;
        e->cap = want;
    }
    e->buf[e->len++] = b;
}

static void j_put16(struct jenc *e, unsigned v) {
    j_put(e, (uint8_t)(v >> 8));
    j_put(e, (uint8_t)v);
}

static void j_put_n(struct jenc *e, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) j_put(e, d[i]);
}

// **A 0xFF IN ENTROPY DATA IS STUFFED WITH A 0x00.** Without it the
// decoder reads the next byte as a marker and the scan ends early --
// a file that is valid for the first few thousand blocks and then
// truncates, which is exactly the shape of bug that looks like the DCT.
static void j_put_bits(struct jenc *e, unsigned value, int nbits) {
    if (nbits <= 0) return;
    e->bitbuf |= (value & ((1u << nbits) - 1)) << (32 - e->bitcnt - nbits);
    e->bitcnt += nbits;
    while (e->bitcnt >= 8) {
        uint8_t b = (uint8_t)(e->bitbuf >> 24);
        j_put(e, b);
        if (b == 0xFF) j_put(e, 0x00);
        e->bitbuf <<= 8;
        e->bitcnt -= 8;
    }
}

// The scan ends on a byte boundary, padded with ONE bits: a run of
// zeroes could be read as a valid code and decode into a phantom
// coefficient.
static void j_flush_bits(struct jenc *e) {
    while (e->bitcnt > 0) j_put_bits(e, 1, 1);
}

// --- the forward DCT (IJG jfdctint) -----------------------------------

#define J_CONST_BITS 13
#define J_PASS1_BITS 2
#define J_F_0_298631336  2446
#define J_F_0_390180644  3196
#define J_F_0_541196100  4433
#define J_F_0_765366865  6270
#define J_F_0_899976223  7373
#define J_F_1_175875602  9633
#define J_F_1_501321110 12299
#define J_F_1_847759065 15137
#define J_F_1_961570560 16069
#define J_F_2_053119869 16819
#define J_F_2_562915447 20995
#define J_F_3_072711026 25172
#define J_DESCALE(x, n)  (((x) + ((int32_t)1 << ((n) - 1))) >> (n))

static void j_fdct(int32_t *b) {
    int32_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int32_t tmp10, tmp11, tmp12, tmp13, z1, z2, z3, z4, z5;

    for (int i = 0; i < 8; i++) {
        int32_t *d = b + i * 8;
        tmp0 = d[0] + d[7]; tmp7 = d[0] - d[7];
        tmp1 = d[1] + d[6]; tmp6 = d[1] - d[6];
        tmp2 = d[2] + d[5]; tmp5 = d[2] - d[5];
        tmp3 = d[3] + d[4]; tmp4 = d[3] - d[4];

        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;
        // Multiplies, not <<: these go negative, and shifting a negative
        // value left is undefined in C (found by UBSAN=1).
        d[0] = (tmp10 + tmp11) * (1 << J_PASS1_BITS);
        d[4] = (tmp10 - tmp11) * (1 << J_PASS1_BITS);

        z1 = (tmp12 + tmp13) * J_F_0_541196100;
        d[2] = J_DESCALE(z1 + tmp13 * J_F_0_765366865, J_CONST_BITS - J_PASS1_BITS);
        d[6] = J_DESCALE(z1 - tmp12 * J_F_1_847759065, J_CONST_BITS - J_PASS1_BITS);

        z1 = tmp4 + tmp7; z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6; z4 = tmp5 + tmp7;
        z5 = (z3 + z4) * J_F_1_175875602;
        tmp4 *= J_F_0_298631336; tmp5 *= J_F_2_053119869;
        tmp6 *= J_F_3_072711026; tmp7 *= J_F_1_501321110;
        z1 *= -J_F_0_899976223; z2 *= -J_F_2_562915447;
        z3 *= -J_F_1_961570560; z4 *= -J_F_0_390180644;
        z3 += z5; z4 += z5;
        d[7] = J_DESCALE(tmp4 + z1 + z3, J_CONST_BITS - J_PASS1_BITS);
        d[5] = J_DESCALE(tmp5 + z2 + z4, J_CONST_BITS - J_PASS1_BITS);
        d[3] = J_DESCALE(tmp6 + z2 + z3, J_CONST_BITS - J_PASS1_BITS);
        d[1] = J_DESCALE(tmp7 + z1 + z4, J_CONST_BITS - J_PASS1_BITS);
    }

    for (int i = 0; i < 8; i++) {
        int32_t *d = b + i;
        tmp0 = d[0]  + d[56]; tmp7 = d[0]  - d[56];
        tmp1 = d[8]  + d[48]; tmp6 = d[8]  - d[48];
        tmp2 = d[16] + d[40]; tmp5 = d[16] - d[40];
        tmp3 = d[24] + d[32]; tmp4 = d[24] - d[32];

        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;
        d[0]  = J_DESCALE(tmp10 + tmp11, J_PASS1_BITS);
        d[32] = J_DESCALE(tmp10 - tmp11, J_PASS1_BITS);

        z1 = (tmp12 + tmp13) * J_F_0_541196100;
        d[16] = J_DESCALE(z1 + tmp13 * J_F_0_765366865, J_CONST_BITS + J_PASS1_BITS);
        d[48] = J_DESCALE(z1 - tmp12 * J_F_1_847759065, J_CONST_BITS + J_PASS1_BITS);

        z1 = tmp4 + tmp7; z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6; z4 = tmp5 + tmp7;
        z5 = (z3 + z4) * J_F_1_175875602;
        tmp4 *= J_F_0_298631336; tmp5 *= J_F_2_053119869;
        tmp6 *= J_F_3_072711026; tmp7 *= J_F_1_501321110;
        z1 *= -J_F_0_899976223; z2 *= -J_F_2_562915447;
        z3 *= -J_F_1_961570560; z4 *= -J_F_0_390180644;
        z3 += z5; z4 += z5;
        d[56] = J_DESCALE(tmp4 + z1 + z3, J_CONST_BITS + J_PASS1_BITS);
        d[40] = J_DESCALE(tmp5 + z2 + z4, J_CONST_BITS + J_PASS1_BITS);
        d[24] = J_DESCALE(tmp6 + z2 + z3, J_CONST_BITS + J_PASS1_BITS);
        d[8]  = J_DESCALE(tmp7 + z1 + z4, J_CONST_BITS + J_PASS1_BITS);
    }
}

// --- one block --------------------------------------------------------

// How many bits |v| needs -- JPEG's "category", the S half of every
// run/size symbol.
static int j_category(int v) {
    int a = v < 0 ? -v : v, n = 0;
    while (a) { a >>= 1; n++; }
    return n;
}

// Returns the new DC predictor. `blk` is level-shifted samples in,
// scratch out.
static int j_encode_block(struct jenc *e, int32_t *blk, int tbl, int pred) {
    j_fdct(blk);

    int32_t q[64];
    for (int i = 0; i < 64; i++) {
        // The islow FDCT leaves its output scaled up by 8, so the
        // divisor carries that factor too (IJG's jcdctmgr does the same).
        int32_t d = (int32_t)e->qt[tbl][i] * 8;
        int32_t v = blk[i];
        q[i] = (v < 0) ? -(((-v) + (d >> 1)) / d) : ((v + (d >> 1)) / d);
    }

    int diff = (int)q[0] - pred;
    int s = j_category(diff);
    j_put_bits(e, e->dc[tbl].code[s], e->dc[tbl].len[s]);
    if (s) j_put_bits(e, (unsigned)(diff < 0 ? diff - 1 : diff), s);

    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = (int)q[uimg_jpeg_zigzag[k]];
        if (v == 0) { run++; continue; }
        while (run > 15) {
            j_put_bits(e, e->ac[tbl].code[0xF0], e->ac[tbl].len[0xF0]);  // ZRL
            run -= 16;
        }
        s = j_category(v);
        int sym = (run << 4) | s;
        j_put_bits(e, e->ac[tbl].code[sym], e->ac[tbl].len[sym]);
        j_put_bits(e, (unsigned)(v < 0 ? v - 1 : v), s);
        run = 0;
    }
    if (run) j_put_bits(e, e->ac[tbl].code[0x00], e->ac[tbl].len[0x00]);  // EOB
    return (int)q[0];
}

// --- the file ---------------------------------------------------------

static void j_write_headers(struct jenc *e, int w, int h) {
    j_put16(e, 0xFFD8);                       // SOI

    j_put16(e, 0xFFE0);                       // APP0, JFIF
    j_put16(e, 16);
    j_put_n(e, (const uint8_t *)"JFIF\0", 5);
    j_put16(e, 0x0101);                       // version 1.1
    j_put(e, 0);                              // no density unit
    j_put16(e, 1); j_put16(e, 1);             // 1:1 pixel aspect
    j_put(e, 0); j_put(e, 0);                 // no thumbnail

    for (int t = 0; t < 2; t++) {             // DQT, one per table
        j_put16(e, 0xFFDB);
        j_put16(e, 2 + 1 + 64);
        j_put(e, (uint8_t)t);                 // 8-bit precision, table t
        for (int k = 0; k < 64; k++)
            j_put(e, (uint8_t)e->qt[t][uimg_jpeg_zigzag[k]]);  // DQT is ZIGZAG order
    }

    j_put16(e, 0xFFC0);                       // SOF0, baseline
    j_put16(e, 8 + 3 * 3);
    j_put(e, 8);                              // 8-bit samples
    j_put16(e, (unsigned)h);
    j_put16(e, (unsigned)w);
    j_put(e, 3);
    j_put(e, 1); j_put(e, 0x22); j_put(e, 0); // Y,  2x2 sampling, qt 0
    j_put(e, 2); j_put(e, 0x11); j_put(e, 1); // Cb, 1x1,           qt 1
    j_put(e, 3); j_put(e, 0x11); j_put(e, 1); // Cr

    static const struct { int cls, id; const uint8_t *bits, *vals; int n; } tabs[] = {
        { 0, 0, uimg_jpeg_std_dc_lum_bits, uimg_jpeg_std_dc_vals,     12  },
        { 1, 0, uimg_jpeg_std_ac_lum_bits, uimg_jpeg_std_ac_lum_vals, 162 },
        { 0, 1, uimg_jpeg_std_dc_chr_bits, uimg_jpeg_std_dc_vals,     12  },
        { 1, 1, uimg_jpeg_std_ac_chr_bits, uimg_jpeg_std_ac_chr_vals, 162 },
    };
    for (unsigned i = 0; i < sizeof tabs / sizeof tabs[0]; i++) {
        j_put16(e, 0xFFC4);                   // DHT
        j_put16(e, (unsigned)(2 + 1 + 16 + tabs[i].n));
        j_put(e, (uint8_t)((tabs[i].cls << 4) | tabs[i].id));
        j_put_n(e, tabs[i].bits + 1, 16);
        j_put_n(e, tabs[i].vals, (size_t)tabs[i].n);
    }

    j_put16(e, 0xFFDA);                       // SOS
    j_put16(e, 6 + 2 * 3);
    j_put(e, 3);
    j_put(e, 1); j_put(e, 0x00);              // Y  uses DC 0, AC 0
    j_put(e, 2); j_put(e, 0x11);              // Cb uses DC 1, AC 1
    j_put(e, 3); j_put(e, 0x11);              // Cr
    j_put(e, 0); j_put(e, 63); j_put(e, 0);   // the whole band, no approximation
}

// The colour transform, ITU-R BT.601, the inverse of the decoder's and
// in the same 16-bit fixed point.
#define J_R_Y   19595   // 0.299   * 65536
#define J_G_Y   38470   // 0.587
#define J_B_Y    7471   // 0.114
#define J_R_CB  11056   // 0.168736
#define J_G_CB  21712   // 0.331264
#define J_B_CB  32768   // 0.5
#define J_R_CR  32768   // 0.5
#define J_G_CR  27440   // 0.418688
#define J_B_CR   5328   // 0.081312

// The quality scaling libjpeg applies to the Annex K tables, which is
// what makes "quality 85" mean here what it means everywhere else.
static void j_scale_qt(uint16_t *out, const uint8_t *base, int quality) {
    int scale = (quality < 50) ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        int v = ((int)base[i] * scale + 50) / 100;
        if (v < 1) v = 1;
        if (v > 255) v = 255;
        out[i] = (uint16_t)v;
    }
}

// A sample of `im` with the edges CLAMPED. An MCU past the right or
// bottom edge is padded by repeating the last real pixel rather than
// with black: a black bar inside the final MCU bleeds back over the
// edge through the decoder's chroma filter and shows as a dark fringe.
static inline uint32_t j_at(const struct uimg *im, int x, int y) {
    if (x >= im->w) x = im->w - 1;
    if (y >= im->h) y = im->h - 1;
    return im->px[(size_t)y * im->w + x];
}

int uimg_jpeg_encode(const struct uimg *im, uint8_t **out, size_t *out_len) {
    return uimg_encode_jpeg(im, J_ENC_QUALITY, out, out_len);
}

int uimg_encode_jpeg(const struct uimg *im, int quality, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    if (im->w > 65535 || im->h > 65535)
        FAIL(-EINVAL, "a JPEG cannot be larger than 65535 pixels on a side");

    struct jenc *e = calloc(1, sizeof *e);
    if (!e) FAIL(-ENOMEM, "not enough memory to encode this image");

    j_scale_qt(e->qt[0], j_qt_lum, quality);
    j_scale_qt(e->qt[1], j_qt_chr, quality);
    j_build_enc_huff(&e->dc[0], uimg_jpeg_std_dc_lum_bits, uimg_jpeg_std_dc_vals);
    j_build_enc_huff(&e->ac[0], uimg_jpeg_std_ac_lum_bits, uimg_jpeg_std_ac_lum_vals);
    j_build_enc_huff(&e->dc[1], uimg_jpeg_std_dc_chr_bits, uimg_jpeg_std_dc_vals);
    j_build_enc_huff(&e->ac[1], uimg_jpeg_std_ac_chr_bits, uimg_jpeg_std_ac_chr_vals);

    j_write_headers(e, im->w, im->h);

    int pred_y = 0, pred_cb = 0, pred_cr = 0;
    int32_t by[4][64], bcb[64], bcr[64];

    // 4:2:0, so the MCU is 16x16: four luma blocks and one of each
    // chroma, averaged over each 2x2 of source pixels.
    for (int my = 0; my < (im->h + 15) / 16; my++) {
        for (int mx = 0; mx < (im->w + 15) / 16; mx++) {
            for (int sy = 0; sy < 8; sy++) {
                for (int sx = 0; sx < 8; sx++) {
                    int32_t cb_sum = 0, cr_sum = 0;
                    for (int q = 0; q < 4; q++) {
                        int px = mx * 16 + sx * 2 + (q & 1);
                        int py = my * 16 + sy * 2 + (q >> 1);
                        uint32_t p = j_at(im, px, py);
                        int r = (int)((p >> 16) & 0xFF);
                        int g = (int)((p >> 8) & 0xFF);
                        int b = (int)(p & 0xFF);

                        int yy = (J_R_Y * r + J_G_Y * g + J_B_Y * b) >> 16;
                        cb_sum += 128 + ((-J_R_CB * r - J_G_CB * g + J_B_CB * b) >> 16);
                        cr_sum += 128 + (( J_R_CR * r - J_G_CR * g - J_B_CR * b) >> 16);

                        // Which of the four luma blocks this pixel is in.
                        int bi = ((py - my * 16) >> 3) * 2 + ((px - mx * 16) >> 3);
                        by[bi][((py & 7)) * 8 + (px & 7)] = yy - 128;
                    }
                    bcb[sy * 8 + sx] = ((cb_sum + 2) >> 2) - 128;
                    bcr[sy * 8 + sx] = ((cr_sum + 2) >> 2) - 128;
                }
            }
            for (int i = 0; i < 4; i++)
                pred_y = j_encode_block(e, by[i], 0, pred_y);
            pred_cb = j_encode_block(e, bcb, 1, pred_cb);
            pred_cr = j_encode_block(e, bcr, 1, pred_cr);
            if (e->oom) break;
        }
        if (e->oom) break;
    }

    j_flush_bits(e);
    j_put16(e, 0xFFD9);                       // EOI

    if (e->oom) {
        free(e->buf);
        free(e);
        FAIL(-ENOMEM, "not enough memory to encode this image");
    }
    *out = e->buf;
    *out_len = e->len;
    free(e);
    return 0;
}
