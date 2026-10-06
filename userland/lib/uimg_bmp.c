// BMP -- Windows' device-independent bitmap, read.
//
// EVERY VERSION WINDOWS WRITES, AND OS/2's TWO: the header's own size
// says which it is -- 12 (OS/2 1.x), 16 or 64 (OS/2 2.x), 40 (Windows
// 3), 52 and 56 (Adobe's V2/V3), 108 (V4), 124 (V5) -- and the fields
// this decoder needs sit at the same offsets in all of them past the
// first. 1, 4 and 8 bits through a palette, 16/24/32 direct, bit masks
// (BI_BITFIELDS) and RLE8/RLE4. A BMP wrapping a JPEG or PNG -- a
// printer format -- and OS/2's Huffman and RLE24 are -ENOTSUP.
//
// TWO RULES ARE THE BROWSERS', where the format leaves a gap, rather
// than Pillow's or GDI's:
//
//   * **An RLE pixel the stream skips (a delta, an early end of line) is
//     TRANSPARENT**, as Chromium and Firefox draw it. GDI paints the
//     palette's first colour there, which is a guess about intent.
//   * **An alpha channel that is ZERO EVERYWHERE is opaque.** A 32-bit
//     file with an alpha mask and nothing in it is a writer that never
//     filled the byte; showing nothing would be the reading the file
//     cannot mean. One non-zero alpha anywhere and every value is taken
//     as written.
//
// A 32-bit BI_RGB file's fourth byte is "reserved" and ignored, as
// Windows and Pillow ignore it -- alpha only ever comes from a mask.
#include "lib/uimg.h"
#include <kerrno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void uimg_set_error(const char *msg);
#define BFAIL(code, msg) do { uimg_set_error(msg); return (code); } while (0)

#define BMP_MAX_DIM    16384
#define BMP_MAX_PIXELS (16 * 1024 * 1024)
#define BMP_FILE_HDR   14

enum {
    BI_RGB = 0, BI_RLE8 = 1, BI_RLE4 = 2, BI_BITFIELDS = 3,
    BI_JPEG = 4, BI_PNG = 5, BI_ALPHABITFIELDS = 6,
};

struct bmp_chan {
    int shift, bits;   // bits 0: the channel is absent
};

struct bmp {
    int w, h, top_down, bpp, comp, hdr, os2;
    uint32_t off;          // where the pixels start
    struct bmp_chan ch[4]; // r, g, b, a
    uint32_t pal[256];     // 0xFFRRGGBB; past the file's count, black
    int pal_n;
};

static uint32_t le16(const uint8_t *d) { return (uint32_t)d[0] | ((uint32_t)d[1] << 8); }
static uint32_t le32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static int known_header(uint32_t s) {
    return s == 12 || s == 16 || s == 40 || s == 52 || s == 56 || s == 64 ||
           s == 108 || s == 124;
}

// "BM" is two bytes, which a text file can start with, so the header
// size has to be one some version uses too -- when there are bytes
// enough to see it. A shorter sniff is answered yes and info() decides.
static int bmp_probe(const uint8_t *d, size_t n) {
    if (n < 2 || d[0] != 'B' || d[1] != 'M') return 0;
    return n < 18 || known_header(le32(d + 14));
}

// A mask must be one run of set bits; anything else has no reading.
static int mask_chan(uint32_t m, struct bmp_chan *c) {
    c->shift = c->bits = 0;
    if (!m) return 0;
    while (!(m & 1)) { m >>= 1; c->shift++; }
    while (m & 1) { m >>= 1; c->bits++; }
    return m ? -1 : 0;
}

// A channel of `bits` widened to 8 by REPEATING its bits, so 5-bit 31
// is 255 and not 248 -- what Pillow's BGR;15/16 and libpng's sBIT
// expansion do.
static uint8_t chan_val(uint32_t v, struct bmp_chan c) {
    if (!c.bits) return 0;
    uint32_t x = (uint32_t)((v >> c.shift) & (((uint64_t)1 << c.bits) - 1));
    if (c.bits >= 8) return (uint8_t)(x >> (c.bits - 8));
    uint32_t r = 0;
    int got = 0;
    while (got < 8) { r = (r << c.bits) | x; got += c.bits; }
    return (uint8_t)(r >> (got - 8));
}

static int bmp_parse(const uint8_t *d, size_t n, struct bmp *b) {
    memset(b, 0, sizeof *b);
    if (n < BMP_FILE_HDR + 12)
        BFAIL(-EINVAL, "truncated BMP file (no room for a header)");
    if (d[0] != 'B' || d[1] != 'M')
        BFAIL(-EINVAL, "not a BMP file (no BM magic)");
    b->off = le32(d + 10);
    uint32_t hs = le32(d + 14);
    if (!known_header(hs))
        BFAIL(-EINVAL, "BMP header size matches no version of the format");
    if ((uint64_t)BMP_FILE_HDR + hs > n)
        BFAIL(-EINVAL, "truncated BMP header");
    b->hdr = (int)hs;
    b->os2 = (hs == 12 || hs == 16 || hs == 64);

    const uint8_t *h = d + BMP_FILE_HDR;
    int64_t w, ht;
    uint32_t planes, clr = 0;
    if (hs == 12) {
        w = le16(h + 4);
        ht = le16(h + 6);
        planes = le16(h + 8);
        b->bpp = (int)le16(h + 10);
        b->comp = BI_RGB;
    } else {
        w = (int32_t)le32(h + 4);
        ht = (int32_t)le32(h + 8);
        planes = le16(h + 12);
        b->bpp = (int)le16(h + 14);
        b->comp = hs >= 20 ? (int)le32(h + 16) : BI_RGB;
        clr = hs >= 36 ? le32(h + 32) : 0;
    }
    if (planes != 1)
        BFAIL(-EINVAL, "BMP declares a plane count other than 1");
    if (ht < 0) { b->top_down = 1; ht = -ht; }
    if (w <= 0 || ht == 0)
        BFAIL(-EINVAL, "BMP declares a zero-sized image");
    if (w > BMP_MAX_DIM || ht > BMP_MAX_DIM || w * ht > BMP_MAX_PIXELS)
        BFAIL(-EINVAL, "BMP is larger than this decoder will allocate");
    b->w = (int)w;
    b->h = (int)ht;

    // OS/2 2.x reuses 3 and 4 for its own two compressions.
    if (hs == 16 || hs == 64) {
        if (b->comp == 3) BFAIL(-ENOTSUP, "this build cannot read an OS/2 Huffman-coded BMP");
        if (b->comp == 4) BFAIL(-ENOTSUP, "this build cannot read an OS/2 RLE24 BMP");
    }
    int bpp = b->bpp;
    switch (b->comp) {
    case BI_RGB:
        if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
            BFAIL(-EINVAL, "BMP declares an impossible bit depth");
        if (hs == 12 && (bpp == 16 || bpp == 32))
            BFAIL(-EINVAL, "an OS/2 1.x BMP cannot be 16 or 32 bits");
        break;
    case BI_RLE8:
        if (bpp != 8) BFAIL(-EINVAL, "RLE8 BMP that is not 8 bits");
        break;
    case BI_RLE4:
        if (bpp != 4) BFAIL(-EINVAL, "RLE4 BMP that is not 4 bits");
        break;
    case BI_BITFIELDS:
    case BI_ALPHABITFIELDS:
        if (bpp != 16 && bpp != 32) BFAIL(-EINVAL, "BMP bit masks on a depth other than 16 or 32");
        break;
    case BI_JPEG:
    case BI_PNG:
        BFAIL(-ENOTSUP, "this build cannot read a BMP wrapping a JPEG or PNG (a printer format)");
    default:
        BFAIL(-EINVAL, "BMP declares an unknown compression");
    }
    if ((b->comp == BI_RLE8 || b->comp == BI_RLE4) && b->top_down)
        BFAIL(-EINVAL, "a top-down BMP cannot be RLE-compressed");

    // The masks: the defaults for BI_RGB, else from the file -- inside
    // the header from V2 on, after a Windows 3 header otherwise.
    uint32_t m[4] = { 0, 0, 0, 0 };
    if (b->comp == BI_RGB && bpp == 16) {
        m[0] = 0x7C00; m[1] = 0x03E0; m[2] = 0x001F;
    } else if (b->comp == BI_RGB && bpp == 32) {
        m[0] = 0xFF0000; m[1] = 0xFF00; m[2] = 0xFF;
    } else if (b->comp == BI_BITFIELDS || b->comp == BI_ALPHABITFIELDS) {
        int count = b->comp == BI_ALPHABITFIELDS ? 4 : 3;
        const uint8_t *src;
        if (hs >= 52) {
            src = h + 40;
            if (hs >= 56) count = 4;
        } else {
            src = h + hs;
            if ((uint64_t)(src - d) + 4u * (unsigned)count > n)
                BFAIL(-EINVAL, "truncated BMP bit masks");
        }
        for (int i = 0; i < count; i++) m[i] = le32(src + 4 * i);
        if (bpp == 16) for (int i = 0; i < 4; i++) m[i] &= 0xFFFF;
        if (!m[0] && !m[1] && !m[2])
            BFAIL(-EINVAL, "BMP bit masks select no colour");
    }
    for (int i = 0; i < 4; i++)
        if (mask_chan(m[i], &b->ch[i]) < 0)
            BFAIL(-EINVAL, "BMP bit mask is not one run of bits");

    // The palette, which follows the header (and a Windows 3 header's
    // masks, but a palette image has none).
    for (int i = 0; i < 256; i++) b->pal[i] = 0xFF000000u;
    if (bpp <= 8) {
        uint32_t most = 1u << bpp;
        uint32_t cnt = (clr && clr < most) ? clr : most;
        unsigned esz = hs == 12 ? 3 : 4;
        uint64_t at = (uint64_t)BMP_FILE_HDR + hs;
        // Some writers count 256 entries and store fewer, the pixels
        // starting where the stored ones end: the palette is what fits
        // before bfOffBits, never pixel bytes read as colours.
        uint64_t end = (b->off > at && b->off <= n) ? b->off : n;
        if (end > at && (end - at) / esz < cnt) cnt = (uint32_t)((end - at) / esz);
        if (end <= at || !cnt) BFAIL(-EINVAL, "BMP has no room for its palette");
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *e = d + at + i * esz;
            b->pal[i] = 0xFF000000u | ((uint32_t)e[2] << 16) | ((uint32_t)e[1] << 8) | e[0];
        }
        b->pal_n = (int)cnt;
    }
    if (b->off >= n)
        BFAIL(-EINVAL, "BMP pixel data starts past the end of the file");
    return 0;
}

static const char *version_name(int hs) {
    switch (hs) {
    case 12: return "OS/2 1.x";
    case 16: case 64: return "OS/2 2.x";
    case 40: return "Windows 3";
    case 52: return "V2";
    case 56: return "V3";
    case 108: return "V4";
    default: return "V5";
    }
}

static int bmp_info(const uint8_t *d, size_t n, struct uimg_info *out) {
    struct bmp b;
    int rc = bmp_parse(d, n, &b);
    if (rc < 0) return rc;
    out->w = b.w;
    out->h = b.h;
    out->components = b.ch[3].bits ? 4 : 3;
    out->format = "bmp";
    out->frames = 1;
    const char *how = b.comp == BI_RLE8 ? ", RLE8" : b.comp == BI_RLE4 ? ", RLE4" :
                      (b.comp == BI_BITFIELDS || b.comp == BI_ALPHABITFIELDS) ? ", bit masks" : "";
    // snprintf is not in this file's world (the hostcheck builds it bare).
    char *o = out->detail;
    size_t cap = sizeof out->detail, k = 0;
    const char *parts[6] = { version_name(b.hdr), ", ", 0, "-bit", how,
                             b.ch[3].bits ? ", alpha" : "" };
    char num[4];
    int bpp = b.bpp, nn = 0;
    if (bpp >= 10) num[nn++] = (char)('0' + bpp / 10);
    num[nn++] = (char)('0' + bpp % 10);
    num[nn] = '\0';
    parts[2] = num;
    for (int i = 0; i < 6; i++)
        for (const char *s = parts[i]; *s && k + 1 < cap; s++) o[k++] = *s;
    o[k] = '\0';
    return 0;
}

static void put(const struct bmp *b, uint32_t *px, int x, int y, uint32_t c) {
    if (x < b->w && y < b->h) px[(size_t)(b->h - 1 - y) * b->w + x] = c;
}

// RLE8/RLE4, bottom-up by definition. `y` counts from the bottom row.
static int bmp_rle(const struct bmp *b, const uint8_t *d, size_t n, uint32_t *px) {
    int rle8 = b->comp == BI_RLE8;
    size_t p = b->off;
    int x = 0, y = 0;
    while (y < b->h) {
        if (p + 2 > n) BFAIL(-EINVAL, "BMP's RLE data ends before its end marker");
        int c0 = d[p], c1 = d[p + 1];
        p += 2;
        if (c0) {                                   // a run of one index (or a nibble pair)
            for (int i = 0; i < c0; i++, x++) {
                int idx = rle8 ? c1 : ((i & 1) ? (c1 & 15) : (c1 >> 4));
                put(b, px, x, y, b->pal[idx]);
            }
        } else if (c1 == 0) {                       // end of line
            x = 0;
            y++;
        } else if (c1 == 1) {                       // end of bitmap
            break;
        } else if (c1 == 2) {                       // delta: the skipped pixels stay clear
            if (p + 2 > n) BFAIL(-EINVAL, "BMP's RLE data ends inside a delta");
            x += d[p];
            y += d[p + 1];
            p += 2;
        } else {                                    // c1 indices, as they are
            size_t bytes = rle8 ? (size_t)c1 : (size_t)(c1 + 1) / 2;
            if (p + bytes > n) BFAIL(-EINVAL, "BMP's RLE data ends inside a literal run");
            for (int i = 0; i < c1; i++, x++) {
                int idx = rle8 ? d[p + i] : ((i & 1) ? (d[p + i / 2] & 15) : (d[p + i / 2] >> 4));
                put(b, px, x, y, b->pal[idx]);
            }
            p += (bytes + 1) & ~(size_t)1;          // literal runs are padded to 16 bits
        }
    }
    return 0;
}

static int bmp_decode(const uint8_t *d, size_t n, struct uimg *out) {
    struct bmp b;
    int rc = bmp_parse(d, n, &b);
    if (rc < 0) return rc;

    size_t stride = (((size_t)b.w * b.bpp + 31) / 32) * 4;
    int rle = b.comp == BI_RLE8 || b.comp == BI_RLE4;
    if (!rle && (uint64_t)b.off + (uint64_t)stride * b.h > n)
        BFAIL(-EINVAL, "truncated BMP pixel data");

    // calloc: an RLE file's skipped pixels are 0, which is transparent.
    uint32_t *px = calloc((size_t)b.w * b.h, sizeof *px);
    if (!px) BFAIL(-ENOMEM, "not enough memory for the decoded image");

    if (rle) {
        rc = bmp_rle(&b, d, n, px);
        if (rc < 0) { free(px); return rc; }
    } else {
        for (int y = 0; y < b.h; y++) {
            const uint8_t *row = d + b.off + (size_t)y * stride;
            uint32_t *o = px + (size_t)(b.top_down ? y : b.h - 1 - y) * b.w;
            for (int x = 0; x < b.w; x++) {
                uint32_t v;
                switch (b.bpp) {
                case 1:  o[x] = b.pal[(row[x >> 3] >> (7 - (x & 7))) & 1]; continue;
                case 4:  o[x] = b.pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; continue;
                case 8:  o[x] = b.pal[row[x]]; continue;
                case 24:
                    o[x] = 0xFF000000u | ((uint32_t)row[3 * x + 2] << 16) |
                           ((uint32_t)row[3 * x + 1] << 8) | row[3 * x];
                    continue;
                case 16: v = le16(row + 2 * x); break;
                default: v = le32(row + 4 * x); break;
                }
                uint32_t a = b.ch[3].bits ? chan_val(v, b.ch[3]) : 255;
                o[x] = (a << 24) | ((uint32_t)chan_val(v, b.ch[0]) << 16) |
                       ((uint32_t)chan_val(v, b.ch[1]) << 8) | chan_val(v, b.ch[2]);
            }
        }
    }

    size_t total = (size_t)b.w * b.h;
    int any_alpha = 0, any_clear = 0;
    for (size_t i = 0; i < total; i++) {
        uint32_t a = px[i] >> 24;
        if (a) any_alpha = 1;
        if (a != 255) any_clear = 1;
    }
    // The zero-everywhere rule (top of file) is for an ALPHA MASK only:
    // an RLE file that is all skipped pixels is clear, as drawn.
    if (b.ch[3].bits && !any_alpha) {
        for (size_t i = 0; i < total; i++) px[i] |= 0xFF000000u;
        any_clear = 0;
    }
    out->w = b.w;
    out->h = b.h;
    out->px = px;
    out->has_alpha = any_clear;
    return 0;
}

const struct uimg_codec uimg_codec_bmp = {
    .name   = "bmp",
    .probe  = bmp_probe,
    .info   = bmp_info,
    .decode = bmp_decode,
};
