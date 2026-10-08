// Pixels into RFB rectangles: the viewer's pixel format, Raw and ZRLE
// (RFB 7.6.1 and 7.7.6). See rd.h.
//
// ZRLE PICKS PER 64x64 TILE whichever of its five forms is smallest --
// raw, solid, packed palette, plain RLE, palette RLE -- by computing
// each one's size first, which is what TigerVNC's encoder does too. A
// desktop is mostly flat colour and text, so palette RLE wins on nearly
// every tile and raw almost never does. The tile data then goes through
// the connection's one zlib stream (udeflate_sync()).
#include "remoted/rd.h"
#include "lib/uinflate.h"
#include <stdlib.h>
#include <string.h>

void rd_buf_put(struct rd_buf *b, const void *src, size_t n) {
    if (b->fail) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        while (cap < b->len + n) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) { b->fail = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

void rd_buf_u8(struct rd_buf *b, uint8_t v) { rd_buf_put(b, &v, 1); }
void rd_buf_u16(struct rd_buf *b, uint16_t v) {
    uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    rd_buf_put(b, x, 2);
}
void rd_buf_u32(struct rd_buf *b, uint32_t v) {
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    rd_buf_put(b, x, 4);
}
void rd_buf_free(struct rd_buf *b) { free(b->p); memset(b, 0, sizeof *b); }

// --- the pixel format ---------------------------------------------------

void rd_pixfmt_native(struct rd_pixfmt *pf) {
    memset(pf, 0, sizeof *pf);
    pf->bpp = 32;
    pf->depth = 24;
    pf->true_colour = 1;
    pf->rmax = pf->gmax = pf->bmax = 255;
    pf->rshift = 16;
    pf->gshift = 8;
    pf->bshift = 0;
}

void rd_pixfmt_read(struct rd_pixfmt *pf, const uint8_t b[16]) {
    pf->bpp = b[0];
    pf->depth = b[1];
    pf->big_endian = b[2] != 0;
    pf->true_colour = b[3] != 0;
    pf->rmax = (uint16_t)(b[4] << 8 | b[5]);
    pf->gmax = (uint16_t)(b[6] << 8 | b[7]);
    pf->bmax = (uint16_t)(b[8] << 8 | b[9]);
    pf->rshift = b[10];
    pf->gshift = b[11];
    pf->bshift = b[12];
}

void rd_pixfmt_write(const struct rd_pixfmt *pf, uint8_t b[16]) {
    memset(b, 0, 16);
    b[0] = pf->bpp;
    b[1] = pf->depth;
    b[2] = pf->big_endian;
    b[3] = pf->true_colour;
    b[4] = (uint8_t)(pf->rmax >> 8); b[5] = (uint8_t)pf->rmax;
    b[6] = (uint8_t)(pf->gmax >> 8); b[7] = (uint8_t)pf->gmax;
    b[8] = (uint8_t)(pf->bmax >> 8); b[9] = (uint8_t)pf->bmax;
    b[10] = pf->rshift;
    b[11] = pf->gshift;
    b[12] = pf->bshift;
}

int rd_pixfmt_check(const struct rd_pixfmt *pf) {
    // A colour map (true_colour 0) would need SetColourMapEntries and a
    // palette of our choosing; every viewer still in use asks for true
    // colour, and TigerVNC's 8-bit "low colour" mode is true colour too.
    if (!pf->true_colour) return -1;
    if (pf->bpp != 8 && pf->bpp != 16 && pf->bpp != 32) return -1;
    if (!pf->rmax || !pf->gmax || !pf->bmax) return -1;
    return 0;
}

static uint32_t to_pixel(const struct rd_pixfmt *pf, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    // Eight bits a channel is what nearly every viewer asks for, and the
    // scaling below -- a divide per channel per pixel -- was the
    // encoder's hottest line.
    if ((pf->rmax & pf->gmax & pf->bmax) == 255)
        return (r << pf->rshift) | (g << pf->gshift) | (b << pf->bshift);
    if (pf->rmax != 255) r = (r * pf->rmax + 127) / 255;
    if (pf->gmax != 255) g = (g * pf->gmax + 127) / 255;
    if (pf->bmax != 255) b = (b * pf->bmax + 127) / 255;
    return (r << pf->rshift) | (g << pf->gshift) | (b << pf->bshift);
}

// The pixel's bytes as the viewer reads them.
static int pixel_bytes(const struct rd_pixfmt *pf, uint32_t v, uint8_t out[4]) {
    int n = pf->bpp / 8;
    for (int i = 0; i < n; i++) {
        int sh = pf->big_endian ? 8 * (n - 1 - i) : 8 * i;
        out[i] = (uint8_t)(v >> sh);
    }
    return n;
}

static int bits_of(uint16_t max) {
    int n = 0;
    while (max) { n++; max >>= 1; }
    return n;
}

// ZRLE's CPIXEL: three bytes instead of four when every colour bit sits
// in the low three bytes or the high three (RFB 7.7.6). `*skip` is which
// byte of the four goes: 3 or 0 in the order they are written.
static int cpixel_bytes(const struct rd_pixfmt *pf, int *skip) {
    *skip = -1;
    if (pf->bpp != 32 || pf->depth > 24) return pf->bpp / 8;
    int hi = 0, lo = 32;
    const uint16_t max[3] = { pf->rmax, pf->gmax, pf->bmax };
    const uint8_t sh[3] = { pf->rshift, pf->gshift, pf->bshift };
    for (int i = 0; i < 3; i++) {
        if (sh[i] + bits_of(max[i]) > hi) hi = sh[i] + bits_of(max[i]);
        if (sh[i] < lo) lo = sh[i];
    }
    int low3 = hi <= 24, high3 = lo >= 8;
    if (!low3 && !high3) return 4;
    // Little-endian puts the low byte first; the byte left out is the
    // unused one, wherever the endianness put it.
    if (low3) *skip = pf->big_endian ? 0 : 3;
    else      *skip = pf->big_endian ? 3 : 0;
    return 3;
}

// --- the rectangles -------------------------------------------------------

static void rect_header(struct rd_buf *out, int x, int y, int w, int h, int32_t enc) {
    rd_buf_u16(out, (uint16_t)x);
    rd_buf_u16(out, (uint16_t)y);
    rd_buf_u16(out, (uint16_t)w);
    rd_buf_u16(out, (uint16_t)h);
    rd_buf_u32(out, (uint32_t)enc);
}

static void enc_raw(struct rd_enc *e, const uint32_t *px, int stride, int x, int y,
                    int w, int h, struct rd_buf *out) {
    rect_header(out, x, y, w, h, RD_ENC_RAW);
    uint8_t row[4 * 64];
    for (int j = 0; j < h; j++) {
        const uint32_t *src = px + (size_t)(y + j) * stride + x;
        for (int i = 0; i < w;) {
            int n = 0, chunk = w - i < 64 ? w - i : 64;
            for (int k = 0; k < chunk; k++)
                n += pixel_bytes(&e->pf, to_pixel(&e->pf, src[i + k]), row + n);
            rd_buf_put(out, row, (size_t)n);
            i += chunk;
        }
    }
}

#define TILE 64
#define PAL_MAX 127

// The palette is found through a small open-addressed hash: a linear
// search of up to 127 entries per pixel was most of the encoder's time.
#define PAL_HASH 512

struct tile {
    uint32_t v[TILE * TILE];   // pixel values in the viewer's format
    int n, w, h;
    uint32_t pal[PAL_MAX];
    int npal;                  // > PAL_MAX: too many colours for a palette
    int runs;
    size_t run_bytes;          // the run lengths' own bytes, summed
    int singles;               // runs of length 1
    uint32_t hkey[PAL_HASH];
    int16_t hidx[PAL_HASH];    // -1 empty
};

static unsigned pal_slot(uint32_t v) { return (v * 2654435761u) >> 23; }   // 9 bits

static int pal_index(const struct tile *t, uint32_t v) {
    for (unsigned h = pal_slot(v);; h = (h + 1) & (PAL_HASH - 1)) {
        if (t->hidx[h] < 0) return -1;
        if (t->hkey[h] == v) return t->hidx[h];
    }
}

static void pal_add(struct tile *t, uint32_t v) {
    unsigned h = pal_slot(v);
    while (t->hidx[h] >= 0) {
        if (t->hkey[h] == v) return;
        h = (h + 1) & (PAL_HASH - 1);
    }
    if (t->npal < PAL_MAX) {
        t->hkey[h] = v;
        t->hidx[h] = (int16_t)t->npal;
        t->pal[t->npal] = v;
    }
    t->npal++;   // counts past PAL_MAX: "too many", without storing them
}

static void analyse(struct tile *t) {
    t->npal = 0;
    t->runs = 0;
    t->run_bytes = 0;
    t->singles = 0;
    for (int i = 0; i < PAL_HASH; i++) t->hidx[i] = -1;
    for (int i = 0; i < t->n;) {
        uint32_t v = t->v[i];
        int len = 1;
        while (i + len < t->n && t->v[i + len] == v) len++;
        t->runs++;
        t->run_bytes += (size_t)(len - 1) / 255 + 1;
        if (len == 1) t->singles++;
        if (t->npal <= PAL_MAX) pal_add(t, v);
        i += len;
    }
}

static void put_cpixel(struct rd_buf *b, const struct rd_pixfmt *pf, int cp, int skip,
                       uint32_t v) {
    uint8_t x[4];
    int n = pixel_bytes(pf, v, x);
    if (cp == 3) {
        uint8_t y[3];
        int k = 0;
        for (int i = 0; i < 4; i++) if (i != skip) y[k++] = x[i];
        rd_buf_put(b, y, 3);
    } else {
        rd_buf_put(b, x, (size_t)n);
    }
}

static void put_runlen(struct rd_buf *b, int len) {
    len -= 1;
    while (len >= 255) { rd_buf_u8(b, 255); len -= 255; }
    rd_buf_u8(b, (uint8_t)len);
}

static void zrle_tile(struct rd_enc *e, struct tile *t, int cp, int skip, struct rd_buf *b) {
    analyse(t);
    const struct rd_pixfmt *pf = &e->pf;
    if (t->npal == 1) {                                     // solid
        rd_buf_u8(b, 1);
        put_cpixel(b, pf, cp, skip, t->v[0]);
        return;
    }
    size_t raw = (size_t)t->n * cp;
    size_t plain = (size_t)t->runs * cp + t->run_bytes;
    size_t best = raw, packed = (size_t)-1, prle = (size_t)-1;
    int bits = 0;
    if (t->npal <= 16) {
        bits = t->npal <= 2 ? 1 : t->npal <= 4 ? 2 : 4;
        packed = (size_t)t->npal * cp + (size_t)t->h * (((size_t)t->w * bits + 7) / 8);
    }
    if (t->npal <= PAL_MAX)
        // A palette RLE run of one is a bare index; a longer one is the
        // index with the top bit set, then its length.
        prle = (size_t)t->npal * cp + (size_t)t->runs + t->run_bytes - (size_t)t->singles;
    if (plain < best) best = plain;
    if (packed < best) best = packed;
    if (prle < best) best = prle;

    if (best == packed) {
        rd_buf_u8(b, (uint8_t)t->npal);
        for (int i = 0; i < t->npal; i++) put_cpixel(b, pf, cp, skip, t->pal[i]);
        for (int y = 0; y < t->h; y++) {
            uint8_t acc = 0;
            int nb = 0;
            for (int x = 0; x < t->w; x++) {
                acc = (uint8_t)(acc << bits | pal_index(t, t->v[y * t->w + x]));
                nb += bits;
                if (nb == 8) { rd_buf_u8(b, acc); acc = 0; nb = 0; }
            }
            if (nb) rd_buf_u8(b, (uint8_t)(acc << (8 - nb)));   // rows are byte-padded
        }
    } else if (best == prle) {
        rd_buf_u8(b, (uint8_t)(128 + t->npal));
        for (int i = 0; i < t->npal; i++) put_cpixel(b, pf, cp, skip, t->pal[i]);
        for (int i = 0; i < t->n;) {
            int len = 1;
            while (i + len < t->n && t->v[i + len] == t->v[i]) len++;
            int idx = pal_index(t, t->v[i]);
            if (len == 1) {
                rd_buf_u8(b, (uint8_t)idx);
            } else {
                rd_buf_u8(b, (uint8_t)(idx | 128));
                put_runlen(b, len);
            }
            i += len;
        }
    } else if (best == plain) {
        rd_buf_u8(b, 128);
        for (int i = 0; i < t->n;) {
            int len = 1;
            while (i + len < t->n && t->v[i + len] == t->v[i]) len++;
            put_cpixel(b, pf, cp, skip, t->v[i]);
            put_runlen(b, len);
            i += len;
        }
    } else {
        rd_buf_u8(b, 0);
        for (int i = 0; i < t->n; i++) put_cpixel(b, pf, cp, skip, t->v[i]);
    }
}

static void enc_zrle(struct rd_enc *e, const uint32_t *px, int stride, int x, int y,
                     int w, int h, struct rd_buf *out) {
    static struct tile t;
    int skip;
    int cp = cpixel_bytes(&e->pf, &skip);
    struct rd_buf *s = &e->scratch;
    s->len = 0;
    for (int ty = 0; ty < h; ty += TILE) {
        for (int tx = 0; tx < w; tx += TILE) {
            t.w = w - tx < TILE ? w - tx : TILE;
            t.h = h - ty < TILE ? h - ty : TILE;
            t.n = t.w * t.h;
            int native = e->pf.rshift == 16 && e->pf.gshift == 8 && e->pf.bshift == 0 &&
                         (e->pf.rmax & e->pf.gmax & e->pf.bmax) == 255;
            for (int j = 0; j < t.h; j++) {
                const uint32_t *src = px + (size_t)(y + ty + j) * stride + x + tx;
                uint32_t *dst = t.v + j * t.w;
                if (native) for (int i = 0; i < t.w; i++) dst[i] = src[i] & 0xFFFFFF;
                else        for (int i = 0; i < t.w; i++) dst[i] = to_pixel(&e->pf, src[i]);
            }
            zrle_tile(e, &t, cp, skip, s);
        }
    }
    if (s->fail) { out->fail = 1; return; }

    size_t cap = udeflate_bound(s->len) + 16;
    uint8_t *z = malloc(cap);
    if (!z) { out->fail = 1; return; }
    struct udeflate_stream st = { e->zrle_started };
    size_t zlen = 0;
    if (udeflate_sync(&st, s->p, s->len, z, cap, &zlen) != 0) {
        free(z);
        out->fail = 1;
        return;
    }
    e->zrle_started = st.started;
    rect_header(out, x, y, w, h, RD_ENC_ZRLE);
    rd_buf_u32(out, (uint32_t)zlen);
    rd_buf_put(out, z, zlen);
    free(z);
}

void rd_enc_rect(struct rd_enc *e, const uint32_t *px, int stride,
                 int x, int y, int w, int h, struct rd_buf *out) {
    if (e->encoding == RD_ENC_ZRLE) enc_zrle(e, px, stride, x, y, w, h, out);
    else                            enc_raw(e, px, stride, x, y, w, h, out);
}

void rd_enc_cursor(struct rd_enc *e, const uint32_t *argb, int w, int h,
                   int hot_x, int hot_y, struct rd_buf *out) {
    rect_header(out, hot_x, hot_y, w, h, -239);
    uint8_t px[4];
    for (int i = 0; i < w * h; i++)
        rd_buf_put(out, px, (size_t)pixel_bytes(&e->pf, to_pixel(&e->pf, argb[i]), px));
    // Drawn where at least half covers: the mask is one bit, so a soft
    // edge rounds to whichever side it is nearer.
    for (int y = 0; y < h; y++) {
        uint8_t acc = 0;
        int nb = 0;
        for (int x = 0; x < w; x++) {
            acc = (uint8_t)(acc << 1 | ((argb[y * w + x] >> 24) >= 128));
            if (++nb == 8) { rd_buf_u8(out, acc); acc = 0; nb = 0; }
        }
        if (nb) rd_buf_u8(out, (uint8_t)(acc << (8 - nb)));
    }
}
