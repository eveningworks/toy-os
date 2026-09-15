// See udeflate.h. The compressing half, ring 3 only.
#include "lib/udeflate.h"
#include <kcrc.h>
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

static const char *g_err = "";
const char *udeflate_error(void) { return g_err; }
#define ZFAIL(code, msg) do { g_err = (msg); return (code); } while (0)

// The length and distance tables are the DECODER's (api/kinflate.h):
// one definition, so the two halves cannot drift into disagreeing about
// what a length code means.
#define len_base   kinflate_len_base
#define len_extra  kinflate_len_extra
#define dist_base  kinflate_dist_base
#define dist_extra kinflate_dist_extra

#define WSIZE KINFLATE_WSIZE

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

static void bw_fixed_sym(struct bitw *b, int sym) {
    if (sym < 144)      bw_bits(b, bit_reverse(0x30 + sym, 8), 8);
    else if (sym < 256) bw_bits(b, bit_reverse(0x190 + sym - 144, 9), 9);
    else if (sym < 280) bw_bits(b, bit_reverse(sym - 256, 7), 7);
    else                bw_bits(b, bit_reverse(0xC0 + sym - 280, 8), 8);
}

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

#define DEF_HBITS  15
#define DEF_HSIZE  (1 << DEF_HBITS)
#define DEF_MINLEN 3
#define DEF_MAXLEN 258
#define DEF_PROBES 32   // chain depth: the whole speed/ratio dial here

static uint32_t def_hash(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2])
           & (DEF_HSIZE - 1);
}

size_t udeflate_bound(size_t n) { return n + n / 8 + 128; }

int udeflate_into(const void *src_v, size_t n, enum kinflate_wrap wrap,
                  void *dst_v, size_t cap, size_t *out_len) {
    const uint8_t *src = src_v;
    uint8_t *dst = dst_v;
    size_t head = 0;

    if (wrap == KINFLATE_ZLIB) {
        if (cap < 6) ZFAIL(-ENOMEM, "no room for a zlib stream");
        dst[0] = 0x78;   // deflate, 32 KiB window
        dst[1] = 0x01;   // and the two bytes are a multiple of 31
        head = 2;
    } else if (wrap == KINFLATE_GZIP) {
        if (cap < 18) ZFAIL(-ENOMEM, "no room for a gzip member");
        static const uint8_t hdr[10] = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 255 };
        memcpy(dst, hdr, sizeof hdr);
        head = 10;
    }

    int32_t *head_tbl = malloc((size_t)DEF_HSIZE * sizeof *head_tbl);
    int32_t *prev = malloc((size_t)WSIZE * sizeof *prev);
    if (!head_tbl || !prev) {
        free(head_tbl); free(prev);
        ZFAIL(-ENOMEM, "no memory for the match tables");
    }
    for (int i = 0; i < DEF_HSIZE; i++) head_tbl[i] = -1;
    for (int i = 0; i < WSIZE; i++) prev[i] = -1;

    size_t tail = (wrap == KINFLATE_RAW) ? 0 : (wrap == KINFLATE_ZLIB ? 4 : 8);
    struct bitw b = { dst + head, cap > head + tail ? cap - head - tail : 0, 0, 0, 0, 0 };
    bw_bits(&b, 1, 1); // BFINAL
    bw_bits(&b, 1, 2); // BTYPE = fixed Huffman

    size_t pos = 0;
    while (pos < n) {
        int best_len = 0, best_dist = 0;
        if (pos + DEF_MINLEN <= n) {
            uint32_t h = def_hash(src + pos);
            int32_t cand = head_tbl[h];
            int probes = DEF_PROBES;
            while (cand >= 0 && probes-- > 0) {
                size_t distance = pos - (size_t)cand;
                if (distance == 0 || distance > WSIZE) break;
                size_t maxl = n - pos;
                if (maxl > DEF_MAXLEN) maxl = DEF_MAXLEN;
                size_t l = 0;
                while (l < maxl && src[cand + l] == src[pos + l]) l++;
                if ((int)l > best_len) {
                    best_len = (int)l;
                    best_dist = (int)distance;
                    if (best_len >= DEF_MAXLEN) break;
                }
                cand = prev[(size_t)cand & (WSIZE - 1)];
            }
        }

        if (best_len >= DEF_MINLEN) {
            bw_match(&b, best_len, best_dist);
            // Every position inside the match still has to enter the
            // chain, or the next search cannot see back past it.
            for (int i = 0; i < best_len; i++) {
                if (pos + DEF_MINLEN <= n) {
                    uint32_t h = def_hash(src + pos);
                    prev[pos & (WSIZE - 1)] = head_tbl[h];
                    head_tbl[h] = (int32_t)pos;
                }
                pos++;
            }
        } else {
            bw_fixed_sym(&b, src[pos]);
            if (pos + DEF_MINLEN <= n) {
                uint32_t h = def_hash(src + pos);
                prev[pos & (WSIZE - 1)] = head_tbl[h];
                head_tbl[h] = (int32_t)pos;
            }
            pos++;
        }
        if (b.full) break;
    }

    bw_fixed_sym(&b, 256); // end of block
    bw_flush(&b);
    free(head_tbl);
    free(prev);
    if (b.full) ZFAIL(-ENOMEM, "the compressed result did not fit");

    size_t len = head + b.len;
    if (wrap == KINFLATE_ZLIB) {
        uint32_t a = 1, s2 = 0;
        for (size_t i = 0; i < n; i++) { a = (a + src[i]) % 65521; s2 = (s2 + a) % 65521; }
        uint32_t sum = (s2 << 16) | a;
        dst[len++] = (uint8_t)(sum >> 24); dst[len++] = (uint8_t)(sum >> 16);
        dst[len++] = (uint8_t)(sum >> 8);  dst[len++] = (uint8_t)sum;
    } else if (wrap == KINFLATE_GZIP) {
        uint32_t crc = kcrc32(src, n);
        dst[len++] = (uint8_t)crc; dst[len++] = (uint8_t)(crc >> 8);
        dst[len++] = (uint8_t)(crc >> 16); dst[len++] = (uint8_t)(crc >> 24);
        uint32_t isz = (uint32_t)n;
        dst[len++] = (uint8_t)isz; dst[len++] = (uint8_t)(isz >> 8);
        dst[len++] = (uint8_t)(isz >> 16); dst[len++] = (uint8_t)(isz >> 24);
    }
    if (out_len) *out_len = len;
    return 0;
}
