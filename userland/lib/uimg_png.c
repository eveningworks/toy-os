// PNG -- read and written.
//
// It arrived as a WRITE-ONLY row in uimg.h's codec table, because a
// screenshot needed a format a host could open and reading one needs
// inflate. The decoder came later, and inflate with it: the compression
// both directions now live in lib/uinflate.h, which is a library rather
// than something private here because /bin/wget inflates a gzip
// Content-Encoding through the same code.
//
// **WHAT IT READS IS 8-BIT, EVERY COLOUR TYPE, NOT INTERLACED.**
// Greyscale, truecolour, palette and both alpha variants, plus tRNS.
// A 1/2/4/16-bit depth or an Adam7 file is answered -ENOTSUP with a
// sentence naming what it found, which is uimg_jpeg.c's arrangement for
// progressive and CMYK -- the file is fine, this build is not, and that
// is a different sentence from "corrupt".
//
// **THE DEFLATE IT WRITES IS FIXED-HUFFMAN LZ77, AND STORED BLOCKS WOULD
// HAVE BEEN A TRAP.** A spec-legal PNG can be written with uncompressed
// deflate blocks in about thirty lines, and a 1920x1080 screenshot then
// weighs 6.2 MB -- a minute per capture over the TFTP link this exists
// to serve. A desktop is mostly flat colour, so a greedy match finder
// takes the same shot to a few hundred KB. What it READS is unaffected
// by that choice: the decoder handles dynamic Huffman blocks, which is
// what every other encoder in the world emits.
//
// **NEITHER HALF IS EVER CHECKED AGAINST THE OTHER.**
// tools/uimg_codec_hostcheck.py has Pillow read what this writes and
// this read what Pillow wrote, because an encoder and decoder that share
// a mistake round-trip perfectly and produce files nothing else opens.
#include "lib/uimg.h"
#include "lib/uinflate.h"
#include <kcrc.h>
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

extern void uimg_set_error(const char *msg);
#define PFAIL(code, msg) do { uimg_set_error(msg); return (code); } while (0)

#define PNG_MAX_DIM 16384

static const uint8_t png_sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };

static int png_probe(const uint8_t *d, size_t n) {
    return n >= 8 && memcmp(d, png_sig, 8) == 0;
}

static uint32_t be32(const uint8_t *d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
           ((uint32_t)d[2] << 8) | (uint32_t)d[3];
}

// IHDR only. It is always the first chunk and always 13 bytes, so this
// needs no chunk walker.
static int png_info(const uint8_t *d, size_t n, struct uimg_info *out) {
    if (!png_probe(d, n)) PFAIL(-EINVAL, "not a PNG file (no signature)");
    if (n < 8 + 8 + 13) PFAIL(-EINVAL, "truncated PNG (no room for an IHDR)");
    if (memcmp(d + 12, "IHDR", 4) != 0)
        PFAIL(-EINVAL, "PNG does not start with an IHDR chunk");

    uint32_t w = be32(d + 16), h = be32(d + 20);
    int depth = d[24], colour = d[25], interlace = d[28];
    if (w == 0 || h == 0) PFAIL(-EINVAL, "PNG declares a zero-sized image");
    if (w > PNG_MAX_DIM || h > PNG_MAX_DIM)
        PFAIL(-EINVAL, "PNG is larger than this build will handle");

    static const char *const kinds[] = {
        "greyscale", "?", "truecolour", "indexed", "greyscale+alpha", "?",
        "truecolour+alpha"
    };
    out->w = (int)w;
    out->h = (int)h;
    out->components = (colour == 2 || colour == 6) ? 3 : 1;
    out->format = "png";

    const char *kind = colour <= 6 ? kinds[colour] : "unknown";
    size_t o = 0;
    for (const char *s = kind; *s && o + 1 < sizeof out->detail; s++)
        out->detail[o++] = *s;
    const char *tail = interlace ? ", interlaced" : "";
    for (const char *s = tail; *s && o + 1 < sizeof out->detail; s++)
        out->detail[o++] = *s;
    out->detail[o] = '\0';
    (void)depth;
    return 0;
}

// --- filtering --------------------------------------------------------

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// Picks a filter per row by the minimum-sum-of-absolute-differences
// heuristic the PNG specification itself recommends, treating each
// filtered byte as signed. It is a heuristic and not a search: trying
// all five and deflating each would cost five compressions a row.
static int choose_filter(const uint8_t *row, const uint8_t *up, int stride,
                          int bpp, uint8_t *out) {
    int best = 0;
    long best_score = -1;
    for (int f = 0; f < 5; f++) {
        long score = 0;
        for (int i = 0; i < stride; i++) {
            int a = i >= bpp ? row[i - bpp] : 0;
            int b = up ? up[i] : 0;
            int c = (i >= bpp && up) ? up[i - bpp] : 0;
            int v;
            switch (f) {
            case 0: v = row[i]; break;
            case 1: v = row[i] - a; break;
            case 2: v = row[i] - b; break;
            case 3: v = row[i] - ((a + b) >> 1); break;
            default: v = row[i] - paeth(a, b, c); break;
            }
            v &= 0xff;
            score += v < 128 ? v : 256 - v;
        }
        if (best_score < 0 || score < best_score) { best_score = score; best = f; }
    }

    out[0] = (uint8_t)best;
    for (int i = 0; i < stride; i++) {
        int a = i >= bpp ? row[i - bpp] : 0;
        int b = up ? up[i] : 0;
        int c = (i >= bpp && up) ? up[i - bpp] : 0;
        int v;
        switch (best) {
        case 0: v = row[i]; break;
        case 1: v = row[i] - a; break;
        case 2: v = row[i] - b; break;
        case 3: v = row[i] - ((a + b) >> 1); break;
        default: v = row[i] - paeth(a, b, c); break;
        }
        out[1 + i] = (uint8_t)v;
    }
    return best;
}

// --- chunks -----------------------------------------------------------

static uint8_t *put_be32(uint8_t *p, uint32_t v) {
    *p++ = (uint8_t)(v >> 24); *p++ = (uint8_t)(v >> 16);
    *p++ = (uint8_t)(v >> 8);  *p++ = (uint8_t)v;
    return p;
}

// A chunk's CRC covers its TYPE as well as its data, and not its length.
// kcrc32 is the reflected zlib/PNG polynomial (api/kcrc.h), which is the
// one PNG means -- not what `cksum` computes.
static uint8_t *put_chunk(uint8_t *p, const char *type, const uint8_t *data,
                          size_t len) {
    p = put_be32(p, (uint32_t)len);
    uint32_t crc = KCRC32_INIT;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)type[i];
    crc = kcrc32_update(crc, p, 4);
    p += 4;
    if (len) {
        memcpy(p, data, len);
        crc = kcrc32_update(crc, p, len);
        p += len;
    }
    return put_be32(p, KCRC32_FINAL(crc));
}

static int png_encode(const struct uimg *im, uint8_t **out, size_t *out_len) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0)
        PFAIL(-EINVAL, "nothing to encode");
    if (im->w > PNG_MAX_DIM || im->h > PNG_MAX_DIM)
        PFAIL(-EINVAL, "larger than this encoder will write");

    int bpp = im->has_alpha ? 4 : 3;
    int colour = im->has_alpha ? 6 : 2;
    size_t stride = (size_t)im->w * bpp;
    size_t raw_len = ((size_t)stride + 1) * (size_t)im->h;

    uint8_t *raw = malloc(raw_len);
    if (!raw) PFAIL(-ENOMEM, "not enough memory to filter the image");

    uint8_t *row = malloc(stride);
    uint8_t *up = malloc(stride);
    if (!row || !up) {
        free(raw); free(row); free(up);
        PFAIL(-ENOMEM, "not enough memory to filter the image");
    }

    for (int y = 0; y < im->h; y++) {
        const uint32_t *src = im->px + (size_t)y * im->w;
        for (int x = 0; x < im->w; x++) {
            uint32_t v = src[x];
            row[(size_t)x * bpp + 0] = (uint8_t)(v >> 16);
            row[(size_t)x * bpp + 1] = (uint8_t)(v >> 8);
            row[(size_t)x * bpp + 2] = (uint8_t)v;
            if (bpp == 4) row[(size_t)x * bpp + 3] = (uint8_t)(v >> 24);
        }
        choose_filter(row, y ? up : NULL, (int)stride, bpp,
                      raw + (size_t)y * (stride + 1));
        memcpy(up, row, stride);
    }
    free(row);

    // THE ZLIB WRAPPER IS THE LIBRARY'S, header and adler32 both -- this
    // file used to build it by hand, and a second copy of a two-byte
    // header whose check bits must be a multiple of 31 is a second place
    // to get it wrong (lib/uinflate.h).
    size_t idat_cap = udeflate_bound(raw_len);
    uint8_t *idat = malloc(idat_cap);
    if (!idat) { free(raw); free(up); PFAIL(-ENOMEM, "not enough memory to compress"); }

    size_t idat_len = 0;
    int rc = udeflate_into(raw, raw_len, UINFLATE_ZLIB, idat, idat_cap, &idat_len);
    free(raw);
    free(up);
    if (rc < 0) {
        free(idat);
        PFAIL(rc, uinflate_error());
    }

    uint8_t ihdr[13];
    uint8_t *q = ihdr;
    q = put_be32(q, (uint32_t)im->w);
    q = put_be32(q, (uint32_t)im->h);
    *q++ = 8;                  // bit depth
    *q++ = (uint8_t)colour;
    *q++ = 0;                  // deflate
    *q++ = 0;                  // adaptive filtering
    *q++ = 0;                  // no interlace

    size_t file_cap = 8 + (12 + 13) + (12 + idat_len) + 12;
    uint8_t *buf = malloc(file_cap);
    if (!buf) { free(idat); PFAIL(-ENOMEM, "not enough memory to assemble the file"); }

    uint8_t *p = buf;
    memcpy(p, png_sig, 8);
    p += 8;
    p = put_chunk(p, "IHDR", ihdr, sizeof ihdr);
    p = put_chunk(p, "IDAT", idat, idat_len);
    p = put_chunk(p, "IEND", NULL, 0);
    free(idat);

    *out = buf;
    *out_len = (size_t)(p - buf);
    return 0;
}

// --- decoding ---------------------------------------------------------
//
// WHAT THIS READS, AND WHAT IT REFUSES. Every colour type -- greyscale,
// truecolour, PALETTE, and both alpha variants -- at 8 bits a channel,
// plus tRNS transparency, not interlaced. A file outside that is
// answered -ENOTSUP with a sentence naming what it found, which is
// uimg_jpeg.c's arrangement for progressive and CMYK and the reason
// -ENOTSUP is distinct from -EINVAL: the file is fine, we are not.
//
// The two left out are 1/2/4/16-bit depths and Adam7 interlacing. Both
// are rare now, and both are where PNG decoders go subtly wrong --
// sub-byte unpacking and a seven-pass walk have plenty of room for an
// off-by-one that produces a picture rather than an error.

#define PNG_MAX_PIXELS (64u * 1024u * 1024u)

struct png_hdr {
    uint32_t w, h;
    int depth, colour, interlace;
    int channels;   // per pixel in the raw stream, before palette expansion
};

static int png_read_ihdr(const uint8_t *d, size_t n, struct png_hdr *h) {
    if (!png_probe(d, n)) PFAIL(-EINVAL, "not a PNG file (no signature)");
    if (n < 8 + 8 + 13 + 4) PFAIL(-EINVAL, "truncated PNG (no room for an IHDR)");
    if (memcmp(d + 12, "IHDR", 4) != 0)
        PFAIL(-EINVAL, "PNG does not start with an IHDR chunk");

    h->w = be32(d + 16);
    h->h = be32(d + 20);
    h->depth = d[24];
    h->colour = d[25];
    h->interlace = d[28];

    if (h->w == 0 || h->h == 0) PFAIL(-EINVAL, "PNG declares a zero-sized image");
    if (h->w > PNG_MAX_DIM || h->h > PNG_MAX_DIM ||
        (uint64_t)h->w * h->h > PNG_MAX_PIXELS)
        PFAIL(-EINVAL, "PNG is larger than this decoder will allocate");
    if (d[26] != 0) PFAIL(-EINVAL, "PNG uses an unknown compression method");
    if (d[27] != 0) PFAIL(-EINVAL, "PNG uses an unknown filter method");

    switch (h->colour) {
    case 0: h->channels = 1; break;   // greyscale
    case 2: h->channels = 3; break;   // truecolour
    case 3: h->channels = 1; break;   // palette index
    case 4: h->channels = 2; break;   // greyscale + alpha
    case 6: h->channels = 4; break;   // truecolour + alpha
    default: PFAIL(-EINVAL, "PNG declares an unknown colour type");
    }

    if (h->depth != 8) {
        if (h->depth == 1 || h->depth == 2 || h->depth == 4 || h->depth == 16)
            PFAIL(-ENOTSUP, "a PNG bit depth other than 8, which this build does not read");
        PFAIL(-EINVAL, "PNG declares an impossible bit depth");
    }
    if (h->interlace == 1)
        PFAIL(-ENOTSUP, "an interlaced PNG, which this build does not read");
    if (h->interlace != 0) PFAIL(-EINVAL, "PNG declares an unknown interlace method");
    return 0;
}

// Undoes one row's filter, in place, against the row above. `a` is the
// pixel to the left, `b` above, `c` above-left -- all zero outside the
// image, which is what makes the first row and first pixel work without
// a special case.
static int png_unfilter(uint8_t *row, const uint8_t *up, size_t stride,
                        int bpp, int filter) {
    for (size_t i = 0; i < stride; i++) {
        int a = i >= (size_t)bpp ? row[i - bpp] : 0;
        int b = up ? up[i] : 0;
        int c = (i >= (size_t)bpp && up) ? up[i - bpp] : 0;
        int v = row[i];
        switch (filter) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: v += paeth(a, b, c); break;
        default: PFAIL(-EINVAL, "PNG uses an unknown row filter");
        }
        row[i] = (uint8_t)v;
    }
    return 0;
}

static int png_decode(const uint8_t *d, size_t n, struct uimg *out) {
    struct png_hdr h;
    int rc = png_read_ihdr(d, n, &h);
    if (rc < 0) return rc;

    // THE PALETTE AND tRNS ARE CHUNKS LIKE ANY OTHER, so the walk has to
    // collect them before the pixels can be expanded -- PLTE always
    // precedes IDAT, and tRNS must too, but a decoder that ASSUMED the
    // order rather than checking would read an empty palette on a file
    // that merely surprised it.
    uint8_t plte[256 * 3];
    int plte_n = 0;
    uint8_t trns[256];
    int trns_n = 0;
    uint16_t trns_grey = 0, trns_r = 0, trns_g = 0, trns_b = 0;
    int have_trns_key = 0;

    // The IDAT chunks are ONE zlib stream split at arbitrary points, so
    // they are concatenated before inflating. A decoder that inflated
    // each chunk separately works on every file written by one encoder
    // and fails on the next.
    uint8_t *z = NULL;
    size_t z_len = 0, z_cap = 0;

    size_t pos = 8;
    int saw_iend = 0;
    while (pos + 8 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t *type = d + pos + 4;
        if (len > n || pos + 12 + len > n) {
            free(z);
            PFAIL(-EINVAL, "a PNG chunk runs past the end of the file");
        }
        const uint8_t *body = d + pos + 8;
        uint32_t crc = be32(d + pos + 8 + len);
        if (kcrc32(type, len + 4) != crc) {
            free(z);
            PFAIL(-EINVAL, "a PNG chunk fails its CRC");
        }

        if (memcmp(type, "PLTE", 4) == 0) {
            if (len % 3 || len > sizeof plte) {
                free(z);
                PFAIL(-EINVAL, "a PNG palette has an impossible size");
            }
            memcpy(plte, body, len);
            plte_n = (int)(len / 3);
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (h.colour == 3) {
                if (len > sizeof trns) { free(z); PFAIL(-EINVAL, "an oversized tRNS"); }
                memcpy(trns, body, len);
                trns_n = (int)len;
            } else if (h.colour == 0 && len >= 2) {
                trns_grey = (uint16_t)((body[0] << 8) | body[1]);
                have_trns_key = 1;
            } else if (h.colour == 2 && len >= 6) {
                trns_r = (uint16_t)((body[0] << 8) | body[1]);
                trns_g = (uint16_t)((body[2] << 8) | body[3]);
                trns_b = (uint16_t)((body[4] << 8) | body[5]);
                have_trns_key = 1;
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (z_len + len > z_cap) {
                size_t want = z_cap ? z_cap * 2 : 16384;
                while (want < z_len + len) want *= 2;
                uint8_t *nz = realloc(z, want);
                if (!nz) { free(z); PFAIL(-ENOMEM, "not enough memory for the compressed data"); }
                z = nz;
                z_cap = want;
            }
            memcpy(z + z_len, body, len);
            z_len += len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            saw_iend = 1;
            break;
        }
        pos += 12 + len;
    }

    if (!saw_iend) { free(z); PFAIL(-EINVAL, "the PNG has no IEND chunk"); }
    if (!z_len) { free(z); PFAIL(-EINVAL, "the PNG has no image data"); }
    if (h.colour == 3 && plte_n == 0) {
        free(z);
        PFAIL(-EINVAL, "a palette PNG with no palette");
    }

    int bpp = h.channels;                       // 8 bits a channel
    size_t stride = (size_t)h.w * bpp;
    size_t raw_len = (stride + 1) * h.h;

    uint8_t *raw = malloc(raw_len);
    if (!raw) { free(z); PFAIL(-ENOMEM, "not enough memory for the decompressed rows"); }

    size_t got = 0;
    rc = uinflate_into(z, z_len, UINFLATE_ZLIB, raw, raw_len, &got);
    free(z);
    if (rc < 0) { free(raw); PFAIL(rc, uinflate_error()); }
    // A SHORT STREAM IS REFUSED, not padded. Half an image decodes to a
    // plausible picture with a grey tail, which reads as a decoder bug
    // rather than as a truncated file (the rule read_file() follows).
    if (got != raw_len) {
        free(raw);
        PFAIL(-EINVAL, "the PNG's image data is the wrong length for its header");
    }

    uint32_t *px = malloc((size_t)h.w * h.h * sizeof *px);
    if (!px) { free(raw); PFAIL(-ENOMEM, "not enough memory for the decoded image"); }

    int transparent = 0;
    for (uint32_t y = 0; y < h.h; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1) + 1;
        uint8_t *up = y ? raw + (size_t)(y - 1) * (stride + 1) + 1 : NULL;
        rc = png_unfilter(row, up, stride, bpp, raw[(size_t)y * (stride + 1)]);
        if (rc < 0) { free(raw); free(px); return rc; }

        for (uint32_t x = 0; x < h.w; x++) {
            const uint8_t *s = row + (size_t)x * bpp;
            uint32_t a = 255, r, g, b;
            switch (h.colour) {
            case 0:
                r = g = b = s[0];
                if (have_trns_key && s[0] == (trns_grey & 0xFF)) a = 0;
                break;
            case 2:
                r = s[0]; g = s[1]; b = s[2];
                if (have_trns_key && s[0] == (trns_r & 0xFF) &&
                    s[1] == (trns_g & 0xFF) && s[2] == (trns_b & 0xFF)) a = 0;
                break;
            case 3: {
                int idx = s[0];
                if (idx >= plte_n) { free(raw); free(px); PFAIL(-EINVAL, "a palette index past the palette"); }
                r = plte[idx * 3]; g = plte[idx * 3 + 1]; b = plte[idx * 3 + 2];
                if (idx < trns_n) a = trns[idx];
                break;
            }
            case 4:
                r = g = b = s[0];
                a = s[1];
                break;
            default:
                r = s[0]; g = s[1]; b = s[2]; a = s[3];
                break;
            }
            if (a != 255) transparent = 1;
            px[(size_t)y * h.w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    free(raw);

    out->w = (int)h.w;
    out->h = (int)h.h;
    out->px = px;
    out->has_alpha = transparent;
    return 0;
}

const struct uimg_codec uimg_codec_png = {
    .name   = "png",
    .probe  = png_probe,
    .info   = png_info,
    .decode = png_decode,
    .encode = png_encode,
};
