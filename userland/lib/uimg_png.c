// PNG -- written, not read.
//
// The asymmetric row in uimg.h's codec table: probe and info work, and
// `decode` is deliberately NULL. PNG is here so a picture can LEAVE this
// machine -- a screenshot a host, a browser or a bug report can open --
// and reading one needs inflate, which is a separate piece of work with
// its own testing pass (docs/roadmap.md). uimg_decode() turns the NULL
// into -ENOTSUP and a sentence saying so, which is a different answer
// from "corrupt file" and the reason that error code exists.
//
// **THE DEFLATE HERE IS FIXED-HUFFMAN LZ77, AND STORED BLOCKS WOULD HAVE
// BEEN A TRAP.** A spec-legal PNG can be written with uncompressed
// deflate blocks in about thirty lines, and a 1920x1080 screenshot then
// weighs 6.2 MB -- which is a minute per capture over the TFTP link this
// exists to serve. A desktop is mostly flat colour, so the filtered rows
// are long runs and a greedy match finder takes the same shot to a few
// hundred KB.
//
// Verified against Python's zlib and Pillow rather than against itself:
// tools/uimg_encode_hostcheck.py decodes what this writes with a foreign
// implementation, which is the only way a bit-packing bug shows up as a
// failure instead of as a file that round-trips through a matching
// mistake (the same argument uimg_qoi.c's header makes about its
// vectors being written by Pillow).
#include "lib/uimg.h"
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

// --- deflate ----------------------------------------------------------

// A bit sink that REFUSES rather than overruns: every write checks, and
// one that does not fit sets `full` and drops the rest. The caller tests
// it once at the end. An encoder that smashed its own heap on an
// unexpectedly incompressible image would be a very quiet bug.
struct bitw {
    uint8_t *buf;
    size_t cap, len;
    uint32_t acc;
    int nacc;
    int full;
};

static void bw_bits(struct bitw *b, uint32_t v, int n) {
    b->acc |= v << b->nacc;
    b->nacc += n;
    while (b->nacc >= 8) {
        if (b->len >= b->cap) { b->full = 1; b->nacc = 0; b->acc = 0; return; }
        b->buf[b->len++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->nacc -= 8;
    }
}

static void bw_flush(struct bitw *b) {
    if (b->nacc > 0) bw_bits(b, 0, 8 - b->nacc);
}

// A Huffman code is packed MOST significant bit first while everything
// else in deflate is least significant bit first, so a code goes out
// reversed. Getting this backwards writes a file that is structurally
// perfect and decodes to noise.
static uint32_t bit_reverse(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

// RFC 1951 3.2.6: the fixed literal/length code.
static void bw_fixed_sym(struct bitw *b, int sym) {
    if (sym < 144)      bw_bits(b, bit_reverse(0x30 + sym, 8), 8);
    else if (sym < 256) bw_bits(b, bit_reverse(0x190 + sym - 144, 9), 9);
    else if (sym < 280) bw_bits(b, bit_reverse(sym - 256, 7), 7);
    else                bw_bits(b, bit_reverse(0xC0 + sym - 280, 8), 8);
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13
};

static void bw_match(struct bitw *b, int len, int dist) {
    int lc = 28;
    while (lc > 0 && len < len_base[lc]) lc--;
    bw_fixed_sym(b, 257 + lc);
    if (len_extra[lc]) bw_bits(b, (uint32_t)(len - len_base[lc]), len_extra[lc]);

    int dc = 29;
    while (dc > 0 && dist < dist_base[dc]) dc--;
    bw_bits(b, bit_reverse((uint32_t)dc, 5), 5);
    if (dist_extra[dc])
        bw_bits(b, (uint32_t)(dist - dist_base[dc]), dist_extra[dc]);
}

#define DEF_WBITS  15
#define DEF_WSIZE  (1 << DEF_WBITS)
#define DEF_HBITS  15
#define DEF_HSIZE  (1 << DEF_HBITS)
#define DEF_MINLEN 3
#define DEF_MAXLEN 258
#define DEF_PROBES 32   // chain depth: the whole speed/ratio dial here

static uint32_t def_hash(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2])
           & (DEF_HSIZE - 1);
}

// Greedy LZ77 into ONE fixed-Huffman block. No lazy matching and no
// dynamic code table: both are worth real ratio on text and close to
// nothing on the long flat runs a screenshot is made of, which the
// match finder already collapses.
static int deflate_fixed(const uint8_t *src, size_t n, uint8_t *dst,
                          size_t dst_cap, size_t *out_len) {
    int32_t *head = malloc((size_t)DEF_HSIZE * sizeof *head);
    int32_t *prev = malloc((size_t)DEF_WSIZE * sizeof *prev);
    if (!head || !prev) { free(head); free(prev); return -ENOMEM; }
    for (int i = 0; i < DEF_HSIZE; i++) head[i] = -1;
    for (int i = 0; i < DEF_WSIZE; i++) prev[i] = -1;

    struct bitw b = { dst, dst_cap, 0, 0, 0, 0 };
    bw_bits(&b, 1, 1); // BFINAL
    bw_bits(&b, 1, 2); // BTYPE = fixed Huffman

    size_t pos = 0;
    while (pos < n) {
        int best_len = 0, best_dist = 0;
        if (pos + DEF_MINLEN <= n) {
            uint32_t h = def_hash(src + pos);
            int32_t cand = head[h];
            int probes = DEF_PROBES;
            while (cand >= 0 && probes-- > 0) {
                size_t dist = pos - (size_t)cand;
                if (dist == 0 || dist > DEF_WSIZE) break;
                size_t maxl = n - pos;
                if (maxl > DEF_MAXLEN) maxl = DEF_MAXLEN;
                size_t l = 0;
                while (l < maxl && src[cand + l] == src[pos + l]) l++;
                if ((int)l > best_len) {
                    best_len = (int)l;
                    best_dist = (int)dist;
                    if (best_len >= DEF_MAXLEN) break;
                }
                cand = prev[(size_t)cand & (DEF_WSIZE - 1)];
            }
        }

        if (best_len >= DEF_MINLEN) {
            bw_match(&b, best_len, best_dist);
            // Every position inside the match still has to enter the
            // chain, or the next search cannot see back past it.
            for (int i = 0; i < best_len; i++) {
                if (pos + DEF_MINLEN <= n) {
                    uint32_t h = def_hash(src + pos);
                    prev[pos & (DEF_WSIZE - 1)] = head[h];
                    head[h] = (int32_t)pos;
                }
                pos++;
            }
        } else {
            bw_fixed_sym(&b, src[pos]);
            if (pos + DEF_MINLEN <= n) {
                uint32_t h = def_hash(src + pos);
                prev[pos & (DEF_WSIZE - 1)] = head[h];
                head[h] = (int32_t)pos;
            }
            pos++;
        }
        if (b.full) break;
    }

    bw_fixed_sym(&b, 256); // end of block
    bw_flush(&b);
    free(head);
    free(prev);
    if (b.full) return -ENOMEM;
    *out_len = b.len;
    return 0;
}

static uint32_t adler32(const uint8_t *d, size_t n) {
    uint32_t a = 1, s = 0;
    // 5552 is the most bytes that cannot overflow the 32-bit sums.
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *d++; s += a; }
        a %= 65521;
        s %= 65521;
    }
    return (s << 16) | a;
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

    // Deflate's worst case is a handful of bits per literal plus the
    // block header; this is comfortably past it.
    size_t z_cap = raw_len + raw_len / 8 + 128;
    uint8_t *z = malloc(z_cap);
    if (!z) { free(raw); free(up); PFAIL(-ENOMEM, "not enough memory to compress"); }

    size_t z_len = 0;
    int rc = deflate_fixed(raw, raw_len, z, z_cap, &z_len);
    if (rc < 0) {
        free(raw); free(up); free(z);
        PFAIL(rc, "not enough memory to compress the image");
    }
    uint32_t sum = adler32(raw, raw_len);
    free(raw);
    free(up);

    // The zlib stream PNG wraps deflate in: 0x78 0x01 is the deflate
    // method with a 32 KiB window, and the two bytes must be a multiple
    // of 31 read big-endian.
    size_t idat_len = 2 + z_len + 4;
    uint8_t *idat = malloc(idat_len);
    if (!idat) { free(z); PFAIL(-ENOMEM, "not enough memory to assemble the file"); }
    idat[0] = 0x78;
    idat[1] = 0x01;
    memcpy(idat + 2, z, z_len);
    put_be32(idat + 2 + z_len, sum);
    free(z);

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

const struct uimg_codec uimg_codec_png = {
    .name   = "png",
    .probe  = png_probe,
    .info   = png_info,
    .decode = NULL,        // see this file's header
    .encode = png_encode,
};
