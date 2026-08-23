// Baseline JPEG, decoded in ring 3.
//
// The first codec behind uimg.h's table. It decodes what a camera, a
// phone or an export dialog actually produces -- baseline sequential
// DCT, Huffman-coded, 8-bit -- and REFUSES everything else by name
// rather than guessing at it:
//
//   * progressive (SOF2) and the other non-baseline SOFn
//   * arithmetic coding (SOF9-SOF15)
//   * 12-bit samples
//   * 4-component CMYK/YCCK
//
// Each of those comes back as -ENOTSUP with a sentence, never as
// -EINVAL, because "this build cannot show progressive JPEGs" and "this
// file is corrupt" are different things to tell a user and the file is
// not at fault in the first case. Progressive is the one worth naming
// twice: it is common on the web, it needs a whole second decoder
// (coefficients arrive across many scans and cannot be IDCTed until the
// last one), and half-decoding it would produce a plausible, wrong
// picture -- the failure mode this project refuses everywhere else too.
//
// NO FLOATING POINT ANYWHERE. There is none in this project, in either
// ring. The IDCT is the standard Loeffler-Ligtenberg-Moschytz
// factorisation in 12-bit fixed point (the constants below carry their
// real values in comments), the colour transform is 16-bit fixed point,
// and the resampler in uimg.c is 16.16.
//
// WHAT IT DOES NOT DO, deliberately, because each is a real quality
// improvement with a real cost and none is needed yet:
//   * fancy (triangle-filter) chroma upsampling -- chroma is replicated,
//     which is what makes a hard red edge on white slightly blocky
//   * IDCT shortcuts beyond the DC-only one
//   * EXIF orientation (the tag is parsed by nobody here, so a photo
//     shot in portrait shows the way its pixels are stored)
#include "lib/uimg.h"
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

// Caps. Every one is a REFUSAL, and they exist because a header field
// is attacker-controlled: a 65535x65535 SOF is four bytes to write and
// 17 GB to allocate.
#define J_MAX_DIM     16384
#define J_MAX_PIXELS  (16 * 1024 * 1024)
#define J_MAX_COMP    3

// The message the last failure left, read through uimg_last_error().
// One string per process, set only on the failing path -- see uimg.h.
extern void uimg_set_error(const char *msg);
#define FAIL(code, msg) do { uimg_set_error(msg); return (code); } while (0)

// --- bit reader -------------------------------------------------------

struct jhuff {
    uint8_t  vals[256];
    int32_t  mincode[17];
    int32_t  maxcode[18];   // -1 = no codes of this length
    int      valptr[17];
    // The first FAST_BITS of the stream, resolved without a loop:
    // (length << 8) | symbol, or 0xFFFF for "longer than this, walk it".
    // A JPEG's common symbols are 2-6 bits, so this is nearly every
    // decode; the slow path exists for correctness, not for speed.
#define J_FAST_BITS 9
    uint16_t fast[1 << J_FAST_BITS];
};

struct jcomp {
    int id;
    int hs, vs;        // sampling factors
    int tq;            // quantisation table index
    int td, ta;        // Huffman table indices, set per scan
    int dcpred;        // DC predictor, reset at every restart interval
    int px_w, px_h;    // plane size, padded out to whole MCUs
    int dw, dh;        // MEANINGFUL samples: the padding past this is
                       // whatever the last block's edge happened to
                       // produce, and an upsampler that treats it as
                       // real bleeds a block's worth of garbage into the
                       // right and bottom edges
    uint8_t *plane;
};

struct jdec {
    const uint8_t *d;
    size_t n, p;

    // Entropy-coded segment state. `marker` is set the moment the bit
    // reader walks into one, which is how the restart handling and the
    // end of a scan are noticed without scanning ahead.
    uint32_t bitbuf;
    int bitcnt, eof_bits, marker;

    uint16_t qt[4][64];      // kept in ZIGZAG order: dequantise, then dezigzag
    struct jhuff hdc[4], hac[4];
    unsigned have_dc, have_ac;

    struct jcomp comp[J_MAX_COMP];
    int ncomp, w, h;
    int hmax, vmax, mcux, mcuy;
    int restart;
    int adobe_transform;     // -1 when there is no Adobe APP14 marker
    int rgb_direct;          // 3 components that are R,G,B rather than YCbCr
    int sof;
};

// JPEG stores a block's 64 coefficients in zigzag order; this maps a
// zigzag index to its place in the 8x8 block.
static const uint8_t j_dezigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// Feeds the bit buffer from the entropy-coded segment.
//
// TWO THINGS MAKE THIS NOT A PLAIN BYTE READ. A 0xFF inside entropy
// data is STUFFED as 0xFF 0x00, so the zero is dropped; and a 0xFF
// followed by anything else IS A MARKER, which ends the segment. Walking
// into a marker parks the reader (`eof_bits`) feeding zero bits from
// then on, leaves `p` pointing AT the 0xFF so the caller can identify
// it, and records which one it was. Feeding zeros rather than failing is
// deliberate: a truncated scan then decodes to a grey tail instead of
// running off the end of the buffer.
static void j_refill(struct jdec *j) {
    while (j->bitcnt <= 24) {
        int b = 0;
        if (!j->eof_bits) {
            if (j->p >= j->n) {
                j->eof_bits = 1;
                j->marker = 0xD9; // treat a truncated file as an implicit EOI
            } else {
                b = j->d[j->p++];
                if (b == 0xFF) {
                    int b2 = (j->p < j->n) ? j->d[j->p] : 0xD9;
                    if (b2 == 0x00) {
                        j->p++;             // stuffed: the 0xFF is data
                    } else {
                        j->p--;             // leave p on the 0xFF
                        j->marker = b2;
                        j->eof_bits = 1;
                        b = 0;
                    }
                }
            }
        }
        j->bitbuf |= (uint32_t)b << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}

static uint32_t j_get_bits(struct jdec *j, int nbits) {
    if (nbits == 0) return 0;
    j_refill(j);
    uint32_t v = j->bitbuf >> (32 - nbits);
    j->bitbuf <<= nbits;
    j->bitcnt -= nbits;
    return v;
}

// JPEG's EXTEND: an `s`-bit magnitude whose top bit is clear is negative.
static int j_extend(uint32_t v, int s) {
    return ((int32_t)v < (1 << (s - 1))) ? (int)v - (1 << s) + 1 : (int)v;
}

static int j_decode_huff(struct jdec *j, const struct jhuff *h) {
    j_refill(j);
    uint32_t look = j->bitbuf >> (32 - J_FAST_BITS);
    uint16_t f = h->fast[look];
    if (f != 0xFFFF) {
        int len = f >> 8;
        j->bitbuf <<= len;
        j->bitcnt -= len;
        return f & 0xFF;
    }
    // Longer than the fast table: walk the canonical code lengths.
    int32_t code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | (int32_t)j_get_bits(j, 1);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l]) {
            int idx = h->valptr[l] + (int)(code - h->mincode[l]);
            if (idx < 0 || idx > 255) return -1;
            return h->vals[idx];
        }
    }
    return -1; // 17 bits with no match: the stream is broken
}

// Builds the canonical code table plus the fast lookup. REFUSES an
// over-subscribed table (more codes than the length allows), which is
// the malformed-input case that would otherwise decode garbage
// confidently.
static int j_build_huff(struct jhuff *h, const uint8_t *counts) {
    int k = 0;
    int32_t code = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        if (counts[l]) {
            code += counts[l];
            k += counts[l];
            h->maxcode[l] = code - 1;
        } else {
            h->maxcode[l] = -1;
        }
        if (code > (1 << l)) return -EINVAL;   // over-subscribed
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;

    for (int i = 0; i < (1 << J_FAST_BITS); i++) h->fast[i] = 0xFFFF;
    k = 0;
    code = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < counts[l]; i++, k++, code++) {
            if (l <= J_FAST_BITS) {
                int32_t base = code << (J_FAST_BITS - l);
                for (int m = 0; m < (1 << (J_FAST_BITS - l)); m++)
                    h->fast[base + m] = (uint16_t)((l << 8) | h->vals[k]);
            }
        }
        code <<= 1;
    }
    return 0;
}

// --- the IDCT ---------------------------------------------------------
//
// One 8x8 block of dequantised coefficients to eight rows of samples.
// The factorisation is Loeffler-Ligtenberg-Moschytz: eleven multiplies
// per 1-D pass instead of the 64 a naive matrix multiply costs, which is
// what every real JPEG decoder uses. Constants are the usual cosines
// scaled by 4096 (12 fractional bits), written as integers because this
// build has no floating point to fold them from.
#define C_0_298631336   1223
#define C_0_390180644  (-1598)
#define C_0_541196100   2217
#define C_0_765366865   3135
#define C_0_899976223  (-3686)
#define C_1_175875602   4816
#define C_1_501321110   6149
#define C_1_847759065  (-7568)
#define C_1_961570560  (-8035)
#define C_2_053119869   8410
#define C_2_562915447 (-10498)
#define C_3_072711026  12586

#define J_IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)          \
    p2 = (s2);                                             \
    p3 = (s6);                                             \
    p1 = (p2 + p3) * C_0_541196100;                        \
    t2 = p1 + p3 * C_1_847759065;                          \
    t3 = p1 + p2 * C_0_765366865;                          \
    p2 = (s0);                                             \
    p3 = (s4);                                             \
    t0 = (p2 + p3) * 4096;                                 \
    t1 = (p2 - p3) * 4096;                                 \
    x0 = t0 + t3;                                          \
    x3 = t0 - t3;                                          \
    x1 = t1 + t2;                                          \
    x2 = t1 - t2;                                          \
    t0 = (s7);                                             \
    t1 = (s5);                                             \
    t2 = (s3);                                             \
    t3 = (s1);                                             \
    p3 = t0 + t2;                                          \
    p4 = t1 + t3;                                          \
    p1 = t0 + t3;                                          \
    p2 = t1 + t2;                                          \
    p5 = (p3 + p4) * C_1_175875602;                        \
    t0 = t0 * C_0_298631336;                               \
    t1 = t1 * C_2_053119869;                               \
    t2 = t2 * C_3_072711026;                               \
    t3 = t3 * C_1_501321110;                               \
    p1 = p5 + p1 * C_0_899976223;                          \
    p2 = p5 + p2 * C_2_562915447;                          \
    p3 = p3 * C_1_961570560;                               \
    p4 = p4 * C_0_390180644;                               \
    t3 += p1 + p4;                                         \
    t2 += p2 + p3;                                         \
    t1 += p2 + p4;                                         \
    t0 += p1 + p3;

static uint8_t j_clamp(int32_t v) {
    if ((uint32_t)v > 255) return v < 0 ? 0 : 255;
    return (uint8_t)v;
}

static void j_idct_block(uint8_t *out, int stride, const int32_t *coef) {
    int32_t val[64];
    int32_t p1, p2, p3, p4, p5, x0, x1, x2, x3, t0, t1, t2, t3;

    // Columns first.
    for (int i = 0; i < 8; i++) {
        const int32_t *d = coef + i;
        int32_t *v = val + i;
        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 &&
            d[40] == 0 && d[48] == 0 && d[56] == 0) {
            // DC only -- the common case in flat areas, and skipping the
            // butterfly for it is the one shortcut here.
            int32_t dc = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
            continue;
        }
        J_IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        v[0]  = (x0 + t3) >> 10;
        v[56] = (x0 - t3) >> 10;
        v[8]  = (x1 + t2) >> 10;
        v[48] = (x1 - t2) >> 10;
        v[16] = (x2 + t1) >> 10;
        v[40] = (x2 - t1) >> 10;
        v[24] = (x3 + t0) >> 10;
        v[32] = (x3 - t0) >> 10;
    }

    // Then rows, with the +128 level shift folded into the rounding
    // constant so the output lands in 0..255 with no separate pass.
    for (int i = 0; i < 8; i++) {
        const int32_t *v = val + i * 8;
        uint8_t *o = out + i * stride;
        J_IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        o[0] = j_clamp((x0 + t3) >> 17);
        o[7] = j_clamp((x0 - t3) >> 17);
        o[1] = j_clamp((x1 + t2) >> 17);
        o[6] = j_clamp((x1 - t2) >> 17);
        o[2] = j_clamp((x2 + t1) >> 17);
        o[5] = j_clamp((x2 - t1) >> 17);
        o[3] = j_clamp((x3 + t0) >> 17);
        o[4] = j_clamp((x3 - t0) >> 17);
    }
}

// --- one block of entropy data ----------------------------------------

static int j_decode_block(struct jdec *j, struct jcomp *c, int32_t *coef) {
    memset(coef, 0, 64 * sizeof *coef);
    const uint16_t *q = j->qt[c->tq];

    int t = j_decode_huff(j, &j->hdc[c->td]);
    if (t < 0 || t > 15) return -EINVAL;
    int diff = t ? j_extend(j_get_bits(j, t), t) : 0;
    c->dcpred += diff;
    coef[0] = (int32_t)c->dcpred * q[0];

    int k = 1;
    do {
        int rs = j_decode_huff(j, &j->hac[c->ta]);
        if (rs < 0) return -EINVAL;
        int s = rs & 15, r = rs >> 4;
        if (s == 0) {
            if (r != 15) break;    // EOB
            k += 16;               // ZRL: sixteen zeroes
        } else {
            k += r;
            if (k > 63) return -EINVAL;
            coef[j_dezigzag[k]] = (int32_t)j_extend(j_get_bits(j, s), s) * q[k];
            k++;
        }
    } while (k < 64);
    return 0;
}

// After a restart interval the bit reader realigns to a byte boundary,
// an RSTn marker is consumed, and every DC predictor resets. A file with
// restart markers that we did NOT honour would drift its DC term and
// produce a picture with sliding brightness bands -- visible, but only
// after the first interval, which is why this is easy to get wrong.
static int j_restart(struct jdec *j) {
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->eof_bits = 0;

    // Skip to the marker: whatever the bit reader stopped on, or the
    // next 0xFF in the stream.
    while (j->p + 1 < j->n && !(j->d[j->p] == 0xFF && j->d[j->p + 1] != 0x00))
        j->p++;
    if (j->p + 1 >= j->n) return -EINVAL;
    int m = j->d[j->p + 1];
    if (m < 0xD0 || m > 0xD7) return -EINVAL;
    j->p += 2;
    j->marker = 0;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].dcpred = 0;
    return 0;
}

// --- headers ----------------------------------------------------------

static int j_u16(const uint8_t *d) { return (d[0] << 8) | d[1]; }

// Parses everything up to and including SOS. On success `*sos_end` is
// the offset of the first entropy-coded byte.
static int j_parse_headers(struct jdec *j, int header_only, size_t *sos_end) {
    j->adobe_transform = -1;
    j->restart = 0;
    j->sof = 0;

    if (j->n < 4 || j->d[0] != 0xFF || j->d[1] != 0xD8)
        FAIL(-EINVAL, "not a JPEG file (no SOI marker)");
    j->p = 2;

    for (;;) {
        // Markers may be preceded by fill bytes (0xFF), and a
        // conforming-enough file sometimes has junk between segments.
        while (j->p < j->n && j->d[j->p] != 0xFF) j->p++;
        while (j->p < j->n && j->d[j->p] == 0xFF) j->p++;
        if (j->p >= j->n) FAIL(-EINVAL, "truncated JPEG (no start of scan)");
        int m = j->d[j->p++];

        if (m == 0xD9) FAIL(-EINVAL, "JPEG ended before any image data");
        if (m >= 0xD0 && m <= 0xD7) continue;  // stray RSTn
        if (m == 0x01) continue;               // TEM, no payload

        if (j->p + 2 > j->n) FAIL(-EINVAL, "truncated JPEG segment header");
        int len = j_u16(j->d + j->p);
        if (len < 2 || j->p + (size_t)len > j->n)
            FAIL(-EINVAL, "JPEG segment runs past the end of the file");
        const uint8_t *seg = j->d + j->p + 2;
        int seglen = len - 2;
        size_t next = j->p + (size_t)len;

        switch (m) {
        case 0xC0: case 0xC1: {          // SOF0 baseline, SOF1 extended sequential
            j->sof = m;
            if (seglen < 6) FAIL(-EINVAL, "malformed SOF segment");
            if (seg[0] != 8)
                FAIL(-ENOTSUP, "12-bit JPEG samples are not supported");
            j->h = j_u16(seg + 1);
            j->w = j_u16(seg + 3);
            j->ncomp = seg[5];
            if (j->w <= 0 || j->h <= 0)
                FAIL(-EINVAL, "JPEG declares a zero-sized image");
            if (j->w > J_MAX_DIM || j->h > J_MAX_DIM ||
                (int64_t)j->w * j->h > J_MAX_PIXELS)
                FAIL(-EINVAL, "JPEG is larger than this decoder will allocate");
            if (j->ncomp == 4)
                FAIL(-ENOTSUP, "CMYK/YCCK JPEGs are not supported");
            if (j->ncomp != 1 && j->ncomp != 3)
                FAIL(-EINVAL, "JPEG has an unusable component count");
            if (seglen < 6 + 3 * j->ncomp) FAIL(-EINVAL, "malformed SOF component list");
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->ncomp; i++) {
                const uint8_t *c = seg + 6 + 3 * i;
                j->comp[i].id = c[0];
                j->comp[i].hs = c[1] >> 4;
                j->comp[i].vs = c[1] & 15;
                j->comp[i].tq = c[2];
                if (j->comp[i].hs < 1 || j->comp[i].hs > 4 ||
                    j->comp[i].vs < 1 || j->comp[i].vs > 4 ||
                    j->comp[i].tq > 3)
                    FAIL(-EINVAL, "JPEG component has impossible sampling factors");
                if (j->comp[i].hs > j->hmax) j->hmax = j->comp[i].hs;
                if (j->comp[i].vs > j->vmax) j->vmax = j->comp[i].vs;
            }
            // A SINGLE-COMPONENT IMAGE HAS NO SUBSAMPLING TO EXPRESS, and
            // an encoder that writes 2x2 there anyway (legal, and some
            // do) would otherwise give us an MCU grid four times too
            // coarse and a picture decoded into the wrong blocks.
            // libjpeg ignores the factors for the same reason.
            if (j->ncomp == 1) {
                j->comp[0].hs = j->comp[0].vs = 1;
                j->hmax = j->vmax = 1;
            }

            // Three components whose IDs spell R, G, B are stored as RGB
            // rather than YCbCr -- rare, produced by some encoders, and
            // the JFIF-less convention libjpeg also applies.
            j->rgb_direct = (j->ncomp == 3 && j->comp[0].id == 'R' &&
                             j->comp[1].id == 'G' && j->comp[2].id == 'B');
            break;
        }
        case 0xC2:
            FAIL(-ENOTSUP, "progressive JPEG is not supported by this build");
        case 0xC3: case 0xC5: case 0xC6: case 0xC7:
            FAIL(-ENOTSUP, "lossless/differential JPEG is not supported");
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            FAIL(-ENOTSUP, "arithmetic-coded JPEG is not supported");

        case 0xC4: {                      // DHT
            int o = 0;
            while (o + 17 <= seglen) {
                int tc = seg[o] >> 4, th = seg[o] & 15;
                if (tc > 1 || th > 3) FAIL(-EINVAL, "malformed Huffman table id");
                uint8_t counts[17];
                counts[0] = 0;
                int total = 0;
                for (int i = 1; i <= 16; i++) {
                    counts[i] = seg[o + i];
                    total += counts[i];
                }
                if (total > 256 || o + 17 + total > seglen)
                    FAIL(-EINVAL, "Huffman table runs past its segment");
                struct jhuff *h = tc ? &j->hac[th] : &j->hdc[th];
                memcpy(h->vals, seg + o + 17, (size_t)total);
                int rc = j_build_huff(h, counts);
                if (rc < 0) FAIL(rc, "over-subscribed Huffman table");
                if (tc) j->have_ac |= 1u << th; else j->have_dc |= 1u << th;
                o += 17 + total;
            }
            break;
        }
        case 0xDB: {                      // DQT
            int o = 0;
            while (o < seglen) {
                int pq = seg[o] >> 4, tq = seg[o] & 15;
                if (tq > 3 || pq > 1) FAIL(-EINVAL, "malformed quantisation table id");
                int need = 1 + (pq ? 128 : 64);
                if (o + need > seglen) FAIL(-EINVAL, "quantisation table runs past its segment");
                for (int i = 0; i < 64; i++)
                    j->qt[tq][i] = pq ? (uint16_t)j_u16(seg + o + 1 + 2 * i)
                                      : seg[o + 1 + i];
                o += need;
            }
            break;
        }
        case 0xDD:                        // DRI
            if (seglen < 2) FAIL(-EINVAL, "malformed restart-interval segment");
            j->restart = j_u16(seg);
            break;

        case 0xEE:                        // APP14, Adobe
            if (seglen >= 12 && memcmp(seg, "Adobe", 5) == 0)
                j->adobe_transform = seg[11];
            break;

        case 0xDA: {                      // SOS
            if (!j->sof) FAIL(-EINVAL, "JPEG scan before any frame header");
            if (seglen < 1) FAIL(-EINVAL, "malformed scan header");
            int ns = seg[0];
            if (ns < 1 || ns > j->ncomp || seglen < 1 + 2 * ns + 3)
                FAIL(-EINVAL, "malformed scan component list");
            // A baseline file has exactly one scan and it names every
            // component; a single-component scan is legal and is handled
            // as the non-interleaved case below.
            for (int i = 0; i < ns; i++) {
                int cid = seg[1 + 2 * i], tt = seg[2 + 2 * i];
                int found = -1;
                for (int k = 0; k < j->ncomp; k++)
                    if (j->comp[k].id == cid) found = k;
                if (found < 0) FAIL(-EINVAL, "scan names a component the frame does not have");
                j->comp[found].td = tt >> 4;
                j->comp[found].ta = tt & 15;
                if (j->comp[found].td > 3 || j->comp[found].ta > 3)
                    FAIL(-EINVAL, "scan names an impossible Huffman table");
                if (!(j->have_dc & (1u << j->comp[found].td)) ||
                    !(j->have_ac & (1u << j->comp[found].ta)))
                    FAIL(-EINVAL, "scan uses a Huffman table the file never defined");
            }
            if (ns != j->ncomp && !header_only)
                FAIL(-ENOTSUP, "multi-scan (non-interleaved) JPEG is not supported");
            *sos_end = next;
            return 0;
        }
        default:
            break;                        // APPn, COM, anything else: skip
        }
        j->p = next;
    }
}

// --- the scan ---------------------------------------------------------

static void j_free_planes(struct jdec *j) {
    for (int i = 0; i < J_MAX_COMP; i++) {
        free(j->comp[i].plane);
        j->comp[i].plane = NULL;
    }
}

static int j_alloc_planes(struct jdec *j) {
    j->mcux = (j->w + j->hmax * 8 - 1) / (j->hmax * 8);
    j->mcuy = (j->h + j->vmax * 8 - 1) / (j->vmax * 8);
    for (int i = 0; i < j->ncomp; i++) {
        struct jcomp *c = &j->comp[i];
        c->px_w = j->mcux * c->hs * 8;
        c->px_h = j->mcuy * c->vs * 8;
        c->dw = (j->w * c->hs + j->hmax - 1) / j->hmax;
        c->dh = (j->h * c->vs + j->vmax - 1) / j->vmax;
        c->plane = malloc((size_t)c->px_w * c->px_h);
        if (!c->plane) { j_free_planes(j); return -ENOMEM; }
    }
    return 0;
}

static int j_decode_scan(struct jdec *j) {
    int32_t coef[64];
    int todo = j->restart ? j->restart : 0x7FFFFFFF;

    for (int i = 0; i < j->ncomp; i++) j->comp[i].dcpred = 0;

    for (int my = 0; my < j->mcuy; my++) {
        for (int mx = 0; mx < j->mcux; mx++) {
            for (int i = 0; i < j->ncomp; i++) {
                struct jcomp *c = &j->comp[i];
                for (int v = 0; v < c->vs; v++) {
                    for (int hh = 0; hh < c->hs; hh++) {
                        int rc = j_decode_block(j, c, coef);
                        if (rc < 0) return rc;
                        int bx = (mx * c->hs + hh) * 8;
                        int by = (my * c->vs + v) * 8;
                        j_idct_block(c->plane + (size_t)by * c->px_w + bx,
                                     c->px_w, coef);
                    }
                }
            }
            if (--todo == 0 && !(my == j->mcuy - 1 && mx == j->mcux - 1)) {
                int rc = j_restart(j);
                if (rc < 0) return rc;
                todo = j->restart;
            }
        }
    }
    return 0;
}

// --- colour -----------------------------------------------------------
//
// YCbCr to RGB, ITU-R BT.601, in 16-bit fixed point:
//   R = Y                 + 1.402   (Cr-128)
//   G = Y - 0.344136 (Cb-128) - 0.714136 (Cr-128)
//   B = Y + 1.772   (Cb-128)
#define YCC_R_CR   91881    // 1.402   * 65536
#define YCC_G_CB   22554    // 0.344136 * 65536
#define YCC_G_CR   46802    // 0.714136 * 65536
#define YCC_B_CB  116130    // 1.772   * 65536

// ONE OUTPUT ROW OF ONE COMPONENT, AT FULL RESOLUTION.
//
// Chroma in a 4:2:0 JPEG is stored at half resolution in each axis and
// has to be stretched back. The obvious way -- repeat each sample --
// is what this decoder did first, and it is visibly wrong: a saturated
// edge (red on white) gains a 2px staircase, because the chroma step
// lands on a block boundary rather than on the edge.
//
// So this is libjpeg's TRIANGLE FILTER ("fancy upsampling"), the
// default in every JPEG decoder anyone has looked at output from:
// each output sample is 3/4 of the nearer stored sample plus 1/4 of the
// next one along, in each axis. Two things follow from matching
// libjpeg's arithmetic rather than inventing an equivalent one. The
// picture is what every other viewer shows. And the difference from
// libjpeg's own output drops to a rounding step, which is what lets
// tools/uimg_hostcheck.py compare against it at a tolerance of 2 --
// a loose tolerance hides real bugs, and replication needed 70.
//
// The edges duplicate rather than extrapolate, again as libjpeg does:
// the first and last stored sample in each axis stands in for the one
// off the end.
static void j_upsample_row(const struct jdec *j, int ci, int y, uint8_t *out) {
    const struct jcomp *c = &j->comp[ci];
    int xs = j->hmax / c->hs, ys = j->vmax / c->vs;
    int dw = c->dw, dh = c->dh;

    if (xs * c->hs != j->hmax || ys * c->vs != j->vmax || xs > 2 || ys > 2) {
        // A sampling factor that is not 1 or 2 relative to the maximum
        // (3x1, say -- legal, and produced by nothing). Replicate: it is
        // correct, merely not pretty, and it keeps this loop bounded.
        int r = y * c->vs / j->vmax;
        if (r > dh - 1) r = dh - 1;
        const uint8_t *in = c->plane + (size_t)r * c->px_w;
        for (int x = 0; x < j->w; x++) {
            int sx = x * c->hs / j->hmax;
            out[x] = in[sx > dw - 1 ? dw - 1 : sx];
        }
        return;
    }

    int r0 = (ys == 2) ? (y >> 1) : y;
    int r1 = (ys == 2) ? ((y & 1) ? r0 + 1 : r0 - 1) : r0;
    if (r0 > dh - 1) r0 = dh - 1;
    if (r1 > dh - 1) r1 = dh - 1;
    if (r1 < 0) r1 = 0;
    const uint8_t *in0 = c->plane + (size_t)r0 * c->px_w;
    const uint8_t *in1 = c->plane + (size_t)r1 * c->px_w;

    if (xs == 1) {
        // Vertical only (4:4:0, or a component already at full width).
        for (int x = 0; x < dw && x < j->w; x++)
            out[x] = (uint8_t)((in0[x] * 3 + in1[x] + 2) >> 2);
        for (int x = dw; x < j->w; x++) out[x] = out[dw - 1];
        return;
    }

    if (ys == 1) {
        // 4:2:2 -- horizontal only. Written out separately rather than
        // falling through the two-row form below because libjpeg's h2v1
        // path rounds differently (+1/+2 against +8/+7 out of sixteen),
        // and one LSB of disagreement is the difference between this
        // decoder matching libjpeg to 3 and to 4. That sounds like
        // pedantry and is not: the tolerance in
        // tools/uimg_hostcheck.py is the only thing standing between a
        // real IDCT bug and a shrug.
        if (dw == 1) {
            for (int x = 0; x < j->w; x++) out[x] = in0[0];
            return;
        }
        if (0 < j->w) out[0] = in0[0];
        if (1 < j->w) out[1] = (uint8_t)((in0[0] * 3 + in0[1] + 2) >> 2);
        for (int i = 1; i < dw - 1; i++) {
            int v = in0[i] * 3;
            if (2 * i < j->w)     out[2 * i]     = (uint8_t)((v + in0[i - 1] + 1) >> 2);
            if (2 * i + 1 < j->w) out[2 * i + 1] = (uint8_t)((v + in0[i + 1] + 2) >> 2);
        }
        int n = dw - 1;
        if (2 * n < j->w)     out[2 * n]     = (uint8_t)((in0[n] * 3 + in0[n - 1] + 1) >> 2);
        if (2 * n + 1 < j->w) out[2 * n + 1] = in0[n];
        for (int x = 2 * dw; x < j->w; x++) out[x] = out[2 * dw - 1];
        return;
    }

    // Horizontal and vertical triangle filter (4:2:0). The
    // The column sums are 3*nearer + 1*farther, so they carry four
    // samples' worth of magnitude and the >> 4 below takes it back out.
    if (dw == 1) {
        int v = (in0[0] * 3 + in1[0] + 2) >> 2;
        for (int x = 0; x < j->w; x++) out[x] = (uint8_t)v;
        return;
    }
    int last, cur, next;
    cur = in0[0] * 3 + in1[0];
    next = in0[1] * 3 + in1[1];
    if (0 < j->w) out[0] = (uint8_t)((cur * 4 + 8) >> 4);
    if (1 < j->w) out[1] = (uint8_t)((cur * 3 + next + 7) >> 4);
    last = cur;
    cur = next;
    for (int i = 1; i < dw - 1; i++) {
        next = in0[i + 1] * 3 + in1[i + 1];
        if (2 * i < j->w)     out[2 * i]     = (uint8_t)((cur * 3 + last + 8) >> 4);
        if (2 * i + 1 < j->w) out[2 * i + 1] = (uint8_t)((cur * 3 + next + 7) >> 4);
        last = cur;
        cur = next;
    }
    int i = dw - 1;
    if (2 * i < j->w)     out[2 * i]     = (uint8_t)((cur * 3 + last + 8) >> 4);
    if (2 * i + 1 < j->w) out[2 * i + 1] = (uint8_t)((cur * 4 + 7) >> 4);
    for (int x = 2 * dw; x < j->w; x++) out[x] = out[2 * dw - 1];
}

static int j_emit(struct jdec *j, struct uimg *out) {
    uint32_t *px = malloc((size_t)j->w * j->h * sizeof *px);
    if (!px) FAIL(-ENOMEM, "not enough memory for the decoded image");

    // One full-width row per component, refilled per output row. This is
    // what the upsampler writes into, so the colour loop below reads
    // three plain arrays and does no sampling arithmetic at all.
    uint8_t *rows[J_MAX_COMP] = { NULL, NULL, NULL };
    for (int i = 0; i < j->ncomp; i++) {
        rows[i] = malloc((size_t)j->w + 2);
        if (!rows[i]) {
            for (int k = 0; k < J_MAX_COMP; k++) free(rows[k]);
            free(px);
            FAIL(-ENOMEM, "not enough memory to decode this image");
        }
    }

    for (int y = 0; y < j->h; y++) {
        uint32_t *row = px + (size_t)y * j->w;
        for (int i = 0; i < j->ncomp; i++) j_upsample_row(j, i, y, rows[i]);

        if (j->ncomp == 1) {
            const uint8_t *ly = rows[0];
            for (int x = 0; x < j->w; x++) {
                uint32_t g = ly[x];
                row[x] = (g << 16) | (g << 8) | g;
            }
            continue;
        }
        const uint8_t *p0 = rows[0], *p1 = rows[1], *p2 = rows[2];
        for (int x = 0; x < j->w; x++) {
            int a = p0[x], b = p1[x], c = p2[x];
            int r, g, bl;
            if (j->rgb_direct || j->adobe_transform == 0) {
                r = a; g = b; bl = c;
            } else {
                int cb = b - 128, cr = c - 128;
                r  = a + ((YCC_R_CR * cr) >> 16);
                g  = a - ((YCC_G_CB * cb + YCC_G_CR * cr) >> 16);
                bl = a + ((YCC_B_CB * cb) >> 16);
            }
            row[x] = ((uint32_t)j_clamp(r) << 16) |
                     ((uint32_t)j_clamp(g) << 8) |
                      (uint32_t)j_clamp(bl);
        }
    }
    for (int k = 0; k < J_MAX_COMP; k++) free(rows[k]);

    out->w = j->w;
    out->h = j->h;
    out->px = px;
    return 0;
}

// --- the codec interface ----------------------------------------------

static int jpeg_probe(const uint8_t *d, size_t n) {
    return n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
}

// Names the subsampling the way every photo tool does -- "4:2:0", not
// "hs=2 vs=2" -- because that is the form anyone comparing this against
// another decoder's output will have in front of them.
static void jpeg_describe(const struct jdec *j, char *out, size_t cap) {
    const char *base = (j->sof == 0xC0) ? "baseline" : "extended sequential";
    const char *sub = "";
    if (j->ncomp == 1) {
        sub = ", grayscale";
    } else if (j->hmax == 1 && j->vmax == 1) {
        sub = ", 4:4:4";
    } else if (j->hmax == 2 && j->vmax == 1) {
        sub = ", 4:2:2";
    } else if (j->hmax == 2 && j->vmax == 2) {
        sub = ", 4:2:0";
    } else if (j->hmax == 1 && j->vmax == 2) {
        sub = ", 4:4:0";
    } else {
        sub = ", unusual subsampling";
    }
    size_t o = 0;
    for (const char *s = base; *s && o + 1 < cap; s++) out[o++] = *s;
    for (const char *s = sub; *s && o + 1 < cap; s++) out[o++] = *s;
    const char *tail = j->restart ? ", restart markers" : "";
    for (const char *s = tail; *s && o + 1 < cap; s++) out[o++] = *s;
    out[o] = '\0';
}

static int jpeg_info(const uint8_t *d, size_t n, struct uimg_info *out) {
    struct jdec *j = calloc(1, sizeof *j);
    if (!j) FAIL(-ENOMEM, "not enough memory to parse this image");
    j->d = d;
    j->n = n;

    size_t sos_end;
    int rc = j_parse_headers(j, 1, &sos_end);
    if (rc == 0) {
        out->w = j->w;
        out->h = j->h;
        out->components = j->ncomp;
        out->format = "jpeg";
        jpeg_describe(j, out->detail, sizeof out->detail);
    }
    free(j);
    return rc;
}

static int jpeg_decode(const uint8_t *d, size_t n, struct uimg *out) {
    struct jdec *j = calloc(1, sizeof *j);
    if (!j) FAIL(-ENOMEM, "not enough memory to decode this image");
    j->d = d;
    j->n = n;

    size_t sos_end;
    int rc = j_parse_headers(j, 0, &sos_end);
    if (rc < 0) { free(j); return rc; }

    rc = j_alloc_planes(j);
    if (rc < 0) { free(j); FAIL(-ENOMEM, "not enough memory for this image's size"); }

    j->p = sos_end;
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->eof_bits = 0;
    j->marker = 0;

    rc = j_decode_scan(j);
    if (rc < 0) {
        j_free_planes(j);
        free(j);
        FAIL(-EINVAL, "corrupt JPEG entropy-coded data");
    }

    rc = j_emit(j, out);
    j_free_planes(j);
    free(j);
    return rc;
}

const struct uimg_codec uimg_codec_jpeg = {
    .name   = "jpeg",
    .probe  = jpeg_probe,
    .info   = jpeg_info,
    .decode = jpeg_decode,
};
