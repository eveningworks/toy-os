// QOI -- the Quite OK Image format, decoded.
//
// The second codec behind uimg.h's table, and the one ICONS use. It is
// here for what JPEG cannot do rather than for variety:
//
//   * **An alpha channel.** An icon has to sit on a wallpaper. JPEG has
//     no transparency at all, so an icon decoded from one arrives in an
//     opaque rectangle.
//   * **Lossless.** At 24-48 pixels an icon is nearly all edges, which
//     is exactly what a DCT rings around. It also makes this decoder
//     testable to the byte: /tests/uimg_test compares QOI output against
//     the reference EXACTLY, where the JPEG vectors have to allow 3.
//
// It is about 150 lines because the format is one page of specification
// (qoiformat.org). Six chunk types, one running 64-entry hash table, no
// entropy coder, no transform, no tables in the file. That is the whole
// reason it was picked over PNG for this: PNG needs inflate before it
// needs anything else, and inflate deserves its own testing pass rather
// than arriving as a prerequisite of icons. PNG stays on the roadmap.
//
// **THE FILES ARE WRITTEN BY PILLOW**, not by anything in this repo
// (tools/gen_icons.py). That is deliberate and is what makes the tests
// mean something: the encoder is a foreign implementation, so a chunk
// type this decoder misreads produces a wrong picture rather than a
// wrong picture that round-trips through a matching bug.
#include "lib/uimg.h"
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

#define QOI_MAX_DIM     16384
#define QOI_MAX_PIXELS  (16 * 1024 * 1024)
#define QOI_HEADER_LEN  14
#define QOI_PADDING_LEN 8

#define QOI_OP_INDEX 0x00 // 00xxxxxx
#define QOI_OP_DIFF  0x40 // 01xxxxxx
#define QOI_OP_LUMA  0x80 // 10xxxxxx
#define QOI_OP_RUN   0xc0 // 11xxxxxx
#define QOI_OP_RGB   0xfe
#define QOI_OP_RGBA  0xff
#define QOI_MASK_2   0xc0

extern void uimg_set_error(const char *msg);
#define QFAIL(code, msg) do { uimg_set_error(msg); return (code); } while (0)

struct qoi_px {
    uint8_t r, g, b, a;
};

// The format's own hash, which is not a general-purpose one and must be
// exactly this: an encoder and a decoder that disagree about it still
// produce a valid-looking picture, wrong only where an INDEX chunk was
// used. Getting it wrong is therefore the subtle failure this codec has,
// and why the vectors include images busy enough to force index hits.
static int qoi_hash(struct qoi_px p) {
    return (p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) % 64;
}

static uint32_t qoi_u32(const uint8_t *d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
           ((uint32_t)d[2] << 8) | (uint32_t)d[3];
}

static int qoi_probe(const uint8_t *d, size_t n) {
    return n >= 4 && d[0] == 'q' && d[1] == 'o' && d[2] == 'i' && d[3] == 'f';
}

// Header only: dimensions, channel count and colourspace, without
// touching a single chunk.
static int qoi_read_header(const uint8_t *d, size_t n, int *w, int *h,
                           int *channels, int *linear) {
    if (n < QOI_HEADER_LEN + QOI_PADDING_LEN)
        QFAIL(-EINVAL, "truncated QOI file (no room for a header)");
    if (!qoi_probe(d, n))
        QFAIL(-EINVAL, "not a QOI file (no qoif magic)");

    uint32_t ww = qoi_u32(d + 4), hh = qoi_u32(d + 8);
    int ch = d[12], cs = d[13];
    if (ww == 0 || hh == 0)
        QFAIL(-EINVAL, "QOI declares a zero-sized image");
    if (ww > QOI_MAX_DIM || hh > QOI_MAX_DIM ||
        (uint64_t)ww * hh > QOI_MAX_PIXELS)
        QFAIL(-EINVAL, "QOI is larger than this decoder will allocate");
    if (ch != 3 && ch != 4)
        QFAIL(-EINVAL, "QOI declares an impossible channel count");
    if (cs > 1)
        QFAIL(-EINVAL, "QOI declares an unknown colourspace");

    *w = (int)ww;
    *h = (int)hh;
    *channels = ch;
    *linear = cs;
    return 0;
}

static int qoi_info(const uint8_t *d, size_t n, struct uimg_info *out) {
    int w, h, ch, linear;
    int rc = qoi_read_header(d, n, &w, &h, &ch, &linear);
    if (rc < 0) return rc;

    out->w = w;
    out->h = h;
    out->components = ch;
    out->format = "qoi";
    // The colourspace byte is INFORMATIONAL in QOI -- it says how the
    // channels were produced, not how to decode them, and every decoder
    // including this one ignores it. Reported rather than hidden,
    // because a file claiming linear light and looking washed out is
    // otherwise unexplainable.
    const char *sp = linear ? "all channels linear" : "sRGB with linear alpha";
    const char *al = (ch == 4) ? ", alpha" : "";
    size_t o = 0;
    for (const char *s = sp; *s && o + 1 < sizeof out->detail; s++) out->detail[o++] = *s;
    for (const char *s = al; *s && o + 1 < sizeof out->detail; s++) out->detail[o++] = *s;
    out->detail[o] = '\0';
    return 0;
}

static int qoi_decode(const uint8_t *d, size_t n, struct uimg *out) {
    int w, h, ch, linear;
    int rc = qoi_read_header(d, n, &w, &h, &ch, &linear);
    if (rc < 0) return rc;

    uint32_t *px = malloc((size_t)w * h * sizeof *px);
    if (!px) QFAIL(-ENOMEM, "not enough memory for the decoded image");

    struct qoi_px index[64];
    memset(index, 0, sizeof index);
    struct qoi_px cur = { 0, 0, 0, 255 };

    size_t p = QOI_HEADER_LEN;
    // The last 8 bytes are the end marker and are never chunk data.
    size_t end = n - QOI_PADDING_LEN;
    long total = (long)w * h;
    int run = 0;
    int transparent = 0;

    for (long i = 0; i < total; i++) {
        if (run > 0) {
            run--;
        } else if (p < end) {
            int b1 = d[p++];
            if (b1 == QOI_OP_RGB) {
                if (p + 3 > end) { free(px); QFAIL(-EINVAL, "truncated QOI RGB chunk"); }
                cur.r = d[p++];
                cur.g = d[p++];
                cur.b = d[p++];
            } else if (b1 == QOI_OP_RGBA) {
                if (p + 4 > end) { free(px); QFAIL(-EINVAL, "truncated QOI RGBA chunk"); }
                cur.r = d[p++];
                cur.g = d[p++];
                cur.b = d[p++];
                cur.a = d[p++];
            } else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
                cur = index[b1 & 0x3f];
            } else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
                // Each delta is 2 bits, BIASED BY 2, and wraps in 8 bits.
                cur.r = (uint8_t)(cur.r + ((b1 >> 4) & 0x03) - 2);
                cur.g = (uint8_t)(cur.g + ((b1 >> 2) & 0x03) - 2);
                cur.b = (uint8_t)(cur.b + (b1 & 0x03) - 2);
            } else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
                if (p + 1 > end) { free(px); QFAIL(-EINVAL, "truncated QOI LUMA chunk"); }
                int b2 = d[p++];
                int vg = (b1 & 0x3f) - 32;
                cur.r = (uint8_t)(cur.r + vg - 8 + ((b2 >> 4) & 0x0f));
                cur.g = (uint8_t)(cur.g + vg);
                cur.b = (uint8_t)(cur.b + vg - 8 + (b2 & 0x0f));
            } else { // QOI_OP_RUN
                run = b1 & 0x3f;   // biased by 1: the current pixel counts
            }
            index[qoi_hash(cur)] = cur;
        } else {
            // Out of chunks with pixels still to fill. Not fatal: the
            // rest stays as the last pixel, which is what a truncated
            // file looks like rather than a crash. It IS reported, so a
            // caller can tell a short file from a short image.
            free(px);
            QFAIL(-EINVAL, "QOI ended before the image was complete");
        }

        if (cur.a != 255) transparent = 1;
        px[i] = ((uint32_t)cur.a << 24) | ((uint32_t)cur.r << 16) |
                ((uint32_t)cur.g << 8) | (uint32_t)cur.b;
    }

    out->w = w;
    out->h = h;
    out->px = px;
    out->has_alpha = transparent;
    return 0;
}



// --- encoding ---------------------------------------------------------
//
// The decoder's own rules run backwards, and the ORDER of the tests is
// the format rather than an optimisation: a run beats the index, the
// index beats a delta, and a delta beats a literal. Pick them in any
// other order and the file is still valid and simply bigger.
//
// The one asymmetry worth knowing: a pixel is written into the index by
// BOTH sides for every chunk except a run, so an encoder that updates
// the table on an index hit as well produces a file that decodes to a
// different picture. It is the same hash either way (qoi_hash), which
// is what makes the round trip in /tests/uimg_test exact.
static uint8_t *qoi_write_header(uint8_t *p, int w, int h, int channels) {
    *p++ = 'q'; *p++ = 'o'; *p++ = 'i'; *p++ = 'f';
    *p++ = (uint8_t)(w >> 24); *p++ = (uint8_t)(w >> 16);
    *p++ = (uint8_t)(w >> 8);  *p++ = (uint8_t)w;
    *p++ = (uint8_t)(h >> 24); *p++ = (uint8_t)(h >> 16);
    *p++ = (uint8_t)(h >> 8);  *p++ = (uint8_t)h;
    *p++ = (uint8_t)channels;
    *p++ = 0; // sRGB with a linear alpha channel
    return p;
}

static int qoi_encode(const struct uimg *im, uint8_t **out, size_t *out_len) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0)
        QFAIL(-EINVAL, "nothing to encode");
    if (im->w > QOI_MAX_DIM || im->h > QOI_MAX_DIM ||
        (uint64_t)im->w * im->h > QOI_MAX_PIXELS)
        QFAIL(-EINVAL, "larger than this encoder will write");

    int channels = im->has_alpha ? 4 : 3;
    long total = (long)im->w * im->h;
    // Worst case is one RGBA chunk per pixel; nothing here can exceed it.
    size_t cap = QOI_HEADER_LEN + (size_t)total * 5 + QOI_PADDING_LEN;
    uint8_t *buf = malloc(cap);
    if (!buf) QFAIL(-ENOMEM, "not enough memory to encode the image");

    uint8_t *p = qoi_write_header(buf, im->w, im->h, channels);

    struct qoi_px index[64];
    memset(index, 0, sizeof index);
    struct qoi_px prev = { 0, 0, 0, 255 };
    int run = 0;

    for (long i = 0; i < total; i++) {
        uint32_t v = im->px[i];
        struct qoi_px cur;
        cur.r = (uint8_t)(v >> 16);
        cur.g = (uint8_t)(v >> 8);
        cur.b = (uint8_t)v;
        // A 3-channel file has no alpha to carry, so the source's top
        // byte must not reach the chunk chooser -- an opaque image whose
        // pixels happen to hold 0x00 there would otherwise emit RGBA
        // chunks the header says are not coming.
        cur.a = channels == 4 ? (uint8_t)(v >> 24) : 255;

        if (cur.r == prev.r && cur.g == prev.g && cur.b == prev.b &&
            cur.a == prev.a) {
            run++;
            if (run == 62 || i == total - 1) {
                *p++ = (uint8_t)(QOI_OP_RUN | (run - 1));
                run = 0;
            }
        } else {
            if (run > 0) {
                *p++ = (uint8_t)(QOI_OP_RUN | (run - 1));
                run = 0;
            }
            int h = qoi_hash(cur);
            if (index[h].r == cur.r && index[h].g == cur.g &&
                index[h].b == cur.b && index[h].a == cur.a) {
                *p++ = (uint8_t)(QOI_OP_INDEX | h);
            } else {
                index[h] = cur;
                if (cur.a == prev.a) {
                    // Signed 8-bit differences, which WRAP: the decoder
                    // adds them back in 8 bits, so 250 -> 2 is +8 and
                    // not -248.
                    int8_t vr = (int8_t)(cur.r - prev.r);
                    int8_t vg = (int8_t)(cur.g - prev.g);
                    int8_t vb = (int8_t)(cur.b - prev.b);
                    int8_t vg_r = (int8_t)(vr - vg);
                    int8_t vg_b = (int8_t)(vb - vg);
                    if (vr > -3 && vr < 2 && vg > -3 && vg < 2 &&
                        vb > -3 && vb < 2) {
                        *p++ = (uint8_t)(QOI_OP_DIFF | ((vr + 2) << 4) |
                                          ((vg + 2) << 2) | (vb + 2));
                    } else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 &&
                               vg_b > -9 && vg_b < 8) {
                        *p++ = (uint8_t)(QOI_OP_LUMA | (vg + 32));
                        *p++ = (uint8_t)(((vg_r + 8) << 4) | (vg_b + 8));
                    } else {
                        *p++ = QOI_OP_RGB;
                        *p++ = cur.r; *p++ = cur.g; *p++ = cur.b;
                    }
                } else {
                    *p++ = QOI_OP_RGBA;
                    *p++ = cur.r; *p++ = cur.g; *p++ = cur.b; *p++ = cur.a;
                }
            }
        }
        prev = cur;
    }

    for (int i = 0; i < QOI_PADDING_LEN - 1; i++) *p++ = 0;
    *p++ = 1;

    *out = buf;
    *out_len = (size_t)(p - buf);
    return 0;
}

const struct uimg_codec uimg_codec_qoi = {
    .name   = "qoi",
    .probe  = qoi_probe,
    .info   = qoi_info,
    .decode = qoi_decode,
    .encode = qoi_encode,
};
