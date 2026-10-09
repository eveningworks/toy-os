// MPEG-1 video (ISO/IEC 11172-2) for uvid: Video CD's codec, and the
// one every MPEG decoder since descends from.
//
// THE PIPELINE, per picture: the bitstream's variable-length codes ->
// quantised DCT coefficients in zigzag order -> dequantised -> an 8x8
// inverse DCT -> added to a PREDICTION copied, with half-pixel
// interpolation, from one or two earlier pictures along motion vectors.
// I pictures predict nothing, P pictures predict from the last I or P,
// B pictures from the anchors either side of them.
//
// **B PICTURES ARRIVE BEFORE THE PICTURE THEY ARE SHOWN BEFORE.** Coded
// order I0 P3 B1 B2 is display order I0 B1 B2 P3: so an anchor is held
// back until the next anchor is decoded, and a B is handed out at once.
// Three frame buffers: the two anchors and the B being decoded.
//
// **THE TABLES ARE BIT STRINGS, BUILT INTO LOOKUPS AT FIRST USE**, so
// each can be read against the standard's Annex B line by line; a code
// longer than the first-level width goes through a second table.
// uvid_mpeg1_selftest() checks every table is prefix-free, which is the
// mistake a hand-typed table makes. tools/uvid_hostcheck.py compares
// whole decodes with FFmpeg's.
//
// The bitstream is UNTRUSTED: every read past a picture's end reads
// zeros, every vector is clamped to the frame, every coefficient index
// is bounded, and a slice that goes wrong is abandoned for the next.
#include "lib/uvid_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIM 2048
#define PAD     8               // zero bytes after the bitstream, so a peek never reads past it

// --- the variable-length code tables (Annex B) --------------------------

struct vlc_code { const char *bits; int16_t val; };

enum {
    MB_QUANT = 1, MB_FWD = 2, MB_BWD = 4, MB_PATTERN = 8, MB_INTRA = 16,
    V_STUFF = 0x7000, V_ESCAPE = 0x7001, V_EOB = 0x7002, V_BAD = 0x7fff,
};

#define END { 0, 0 }

static const struct vlc_code T_MBAI[] = {      // B.1, macroblock_address_increment
    { "1", 1 }, { "011", 2 }, { "010", 3 }, { "0011", 4 }, { "0010", 5 },
    { "00011", 6 }, { "00010", 7 }, { "0000111", 8 }, { "0000110", 9 },
    { "00001011", 10 }, { "00001010", 11 }, { "00001001", 12 }, { "00001000", 13 },
    { "00000111", 14 }, { "00000110", 15 }, { "0000010111", 16 }, { "0000010110", 17 },
    { "0000010101", 18 }, { "0000010100", 19 }, { "0000010011", 20 }, { "0000010010", 21 },
    { "00000100011", 22 }, { "00000100010", 23 }, { "00000100001", 24 }, { "00000100000", 25 },
    { "00000011111", 26 }, { "00000011110", 27 }, { "00000011101", 28 }, { "00000011100", 29 },
    { "00000011011", 30 }, { "00000011010", 31 }, { "00000011001", 32 }, { "00000011000", 33 },
    { "00000001111", V_STUFF }, { "00000001000", V_ESCAPE }, END,
};

static const struct vlc_code T_MBTYPE_I[] = {  // B.2a
    { "1", MB_INTRA }, { "01", MB_INTRA | MB_QUANT }, END,
};
static const struct vlc_code T_MBTYPE_P[] = {  // B.2b
    { "1", MB_FWD | MB_PATTERN }, { "01", MB_PATTERN }, { "001", MB_FWD },
    { "00011", MB_INTRA }, { "00010", MB_FWD | MB_PATTERN | MB_QUANT },
    { "00001", MB_PATTERN | MB_QUANT }, { "000001", MB_INTRA | MB_QUANT }, END,
};
static const struct vlc_code T_MBTYPE_B[] = {  // B.2c
    { "10", MB_FWD | MB_BWD }, { "11", MB_FWD | MB_BWD | MB_PATTERN },
    { "010", MB_BWD }, { "011", MB_BWD | MB_PATTERN },
    { "0010", MB_FWD }, { "0011", MB_FWD | MB_PATTERN },
    { "00011", MB_INTRA }, { "00010", MB_FWD | MB_BWD | MB_PATTERN | MB_QUANT },
    { "000011", MB_FWD | MB_PATTERN | MB_QUANT }, { "000010", MB_BWD | MB_PATTERN | MB_QUANT },
    { "000001", MB_INTRA | MB_QUANT }, END,
};
static const struct vlc_code T_MBTYPE_D[] = {  // B.2d
    { "1", MB_INTRA }, END,
};

static const struct vlc_code T_CBP[] = {       // B.3, coded_block_pattern
    { "111", 60 }, { "1101", 4 }, { "1100", 8 }, { "1011", 16 }, { "1010", 32 },
    { "10011", 12 }, { "10010", 48 }, { "10001", 20 }, { "10000", 40 },
    { "01111", 28 }, { "01110", 44 }, { "01101", 52 }, { "01100", 56 },
    { "01011", 1 }, { "01010", 61 }, { "01001", 2 }, { "01000", 62 },
    { "001111", 24 }, { "001110", 36 }, { "001101", 3 }, { "001100", 63 },
    { "0010111", 5 }, { "0010110", 9 }, { "0010101", 17 }, { "0010100", 33 },
    { "0010011", 6 }, { "0010010", 10 }, { "0010001", 18 }, { "0010000", 34 },
    { "00011111", 7 }, { "00011110", 11 }, { "00011101", 19 }, { "00011100", 35 },
    { "00011011", 13 }, { "00011010", 49 }, { "00011001", 21 }, { "00011000", 41 },
    { "00010111", 14 }, { "00010110", 50 }, { "00010101", 22 }, { "00010100", 42 },
    { "00010011", 15 }, { "00010010", 51 }, { "00010001", 23 }, { "00010000", 43 },
    { "00001111", 25 }, { "00001110", 37 }, { "00001101", 26 }, { "00001100", 38 },
    { "00001011", 29 }, { "00001010", 45 }, { "00001001", 53 }, { "00001000", 57 },
    { "00000111", 30 }, { "00000110", 46 }, { "00000101", 54 }, { "00000100", 58 },
    { "000000111", 31 }, { "000000110", 47 }, { "000000101", 55 }, { "000000100", 59 },
    { "000000011", 27 }, { "000000010", 39 }, END,
};

static const struct vlc_code T_MOTION[] = {    // B.4, motion_code magnitude (sign follows)
    { "1", 0 }, { "01", 1 }, { "001", 2 }, { "0001", 3 }, { "000011", 4 },
    { "0000101", 5 }, { "0000100", 6 }, { "0000011", 7 },
    { "000001011", 8 }, { "000001010", 9 }, { "000001001", 10 },
    { "0000010001", 11 }, { "0000010000", 12 }, { "0000001111", 13 },
    { "0000001110", 14 }, { "0000001101", 15 }, { "0000001100", 16 }, END,
};

static const struct vlc_code T_DC_LUMA[] = {   // B.5a, dct_dc_size_luminance
    { "100", 0 }, { "00", 1 }, { "01", 2 }, { "101", 3 }, { "110", 4 },
    { "1110", 5 }, { "11110", 6 }, { "111110", 7 }, { "1111110", 8 }, END,
};
static const struct vlc_code T_DC_CHROMA[] = { // B.5b, dct_dc_size_chrominance
    { "00", 0 }, { "01", 1 }, { "10", 2 }, { "110", 3 }, { "1110", 4 },
    { "11110", 5 }, { "111110", 6 }, { "1111110", 7 }, { "11111110", 8 }, END,
};

// B.5c-f, dct_coeff_first and dct_coeff_next, as (run << 8 | level),
// the sign bit following. "11" is next's (0,1) and "10" its end of
// block; first reads a lone "1" as (0,1) instead (built below).
#define RL(r, l) ((r) << 8 | (l))
static const struct vlc_code T_DCT[] = {
    { "011", RL(1, 1) }, { "0100", RL(0, 2) }, { "0101", RL(2, 1) },
    { "00101", RL(0, 3) }, { "00111", RL(3, 1) }, { "00110", RL(4, 1) },
    { "000110", RL(1, 2) }, { "000111", RL(5, 1) }, { "000101", RL(6, 1) }, { "000100", RL(7, 1) },
    { "0000110", RL(0, 4) }, { "0000100", RL(2, 2) }, { "0000111", RL(8, 1) }, { "0000101", RL(9, 1) },
    { "000001", V_ESCAPE },
    { "00100110", RL(0, 5) }, { "00100001", RL(0, 6) }, { "00100101", RL(1, 3) },
    { "00100100", RL(3, 2) }, { "00100111", RL(10, 1) }, { "00100011", RL(11, 1) },
    { "00100010", RL(12, 1) }, { "00100000", RL(13, 1) },
    { "0000001010", RL(0, 7) }, { "0000001100", RL(1, 4) }, { "0000001011", RL(2, 3) },
    { "0000001111", RL(4, 2) }, { "0000001001", RL(5, 2) }, { "0000001110", RL(14, 1) },
    { "0000001101", RL(15, 1) }, { "0000001000", RL(16, 1) },
    { "000000011101", RL(0, 8) }, { "000000011000", RL(0, 9) }, { "000000010011", RL(0, 10) },
    { "000000010000", RL(0, 11) }, { "000000011011", RL(1, 5) }, { "000000010100", RL(2, 4) },
    { "000000011100", RL(3, 3) }, { "000000010010", RL(4, 3) }, { "000000011110", RL(6, 2) },
    { "000000010101", RL(7, 2) }, { "000000010001", RL(8, 2) }, { "000000011111", RL(17, 1) },
    { "000000011010", RL(18, 1) }, { "000000011001", RL(19, 1) }, { "000000010111", RL(20, 1) },
    { "000000010110", RL(21, 1) },
    { "0000000011010", RL(0, 12) }, { "0000000011001", RL(0, 13) }, { "0000000011000", RL(0, 14) },
    { "0000000010111", RL(0, 15) }, { "0000000010110", RL(1, 6) }, { "0000000010101", RL(1, 7) },
    { "0000000010100", RL(2, 5) }, { "0000000010011", RL(3, 4) }, { "0000000010010", RL(5, 3) },
    { "0000000010001", RL(9, 2) }, { "0000000010000", RL(10, 2) }, { "0000000011111", RL(22, 1) },
    { "0000000011110", RL(23, 1) }, { "0000000011101", RL(24, 1) }, { "0000000011100", RL(25, 1) },
    { "0000000011011", RL(26, 1) },
    { "00000000011111", RL(0, 16) }, { "00000000011110", RL(0, 17) }, { "00000000011101", RL(0, 18) },
    { "00000000011100", RL(0, 19) }, { "00000000011011", RL(0, 20) }, { "00000000011010", RL(0, 21) },
    { "00000000011001", RL(0, 22) }, { "00000000011000", RL(0, 23) }, { "00000000010111", RL(0, 24) },
    { "00000000010110", RL(0, 25) }, { "00000000010101", RL(0, 26) }, { "00000000010100", RL(0, 27) },
    { "00000000010011", RL(0, 28) }, { "00000000010010", RL(0, 29) }, { "00000000010001", RL(0, 30) },
    { "00000000010000", RL(0, 31) },
    { "000000000011000", RL(0, 32) }, { "000000000010111", RL(0, 33) }, { "000000000010110", RL(0, 34) },
    { "000000000010101", RL(0, 35) }, { "000000000010100", RL(0, 36) }, { "000000000010011", RL(0, 37) },
    { "000000000010010", RL(0, 38) }, { "000000000010001", RL(0, 39) }, { "000000000010000", RL(0, 40) },
    { "000000000011111", RL(1, 8) }, { "000000000011110", RL(1, 9) }, { "000000000011101", RL(1, 10) },
    { "000000000011100", RL(1, 11) }, { "000000000011011", RL(1, 12) }, { "000000000011010", RL(1, 13) },
    { "000000000011001", RL(1, 14) },
    { "0000000000010011", RL(1, 15) }, { "0000000000010010", RL(1, 16) }, { "0000000000010001", RL(1, 17) },
    { "0000000000010000", RL(1, 18) }, { "0000000000010100", RL(6, 3) }, { "0000000000011010", RL(11, 2) },
    { "0000000000011001", RL(12, 2) }, { "0000000000011000", RL(13, 2) }, { "0000000000010111", RL(14, 2) },
    { "0000000000010110", RL(15, 2) }, { "0000000000010101", RL(16, 2) }, { "0000000000011111", RL(27, 1) },
    { "0000000000011110", RL(28, 1) }, { "0000000000011101", RL(29, 1) }, { "0000000000011100", RL(30, 1) },
    { "0000000000011011", RL(31, 1) },
    END,
};

// --- table lookups --------------------------------------------------------
//
// An entry is (value << 5 | length) for a code that ends within the
// table's width, or the NEGATIVE of (subtable offset << 5 | its width)
// for one that continues. V_BAD fills what no code reaches.

struct vlc {
    int32_t *lut;
    int bits;               // the first level's width
};

static int code_len(const char *b) { return (int)strlen(b); }

static uint32_t code_bits(const char *b) {
    uint32_t v = 0;
    for (; *b; b++) v = v << 1 | (uint32_t)(*b == '1');
    return v;
}

// Two lists, so dct_coeff_first and _next share one table of codes.
static int vlc_build(struct vlc *v, int bits, const struct vlc_code *a, const struct vlc_code *b) {
    int size = 1 << bits;
    // Per first-level prefix, the widest continuation. Heap, not stack:
    // 4 KiB is twice the ring-3 frame budget.
    int *sub_need = calloc((size_t)size, sizeof *sub_need);
    if (!sub_need) return -ENOMEM;
    const struct vlc_code *lists[2] = { a, b };
    for (int l = 0; l < 2; l++)
        for (const struct vlc_code *c = lists[l]; c && c->bits; c++) {
            int n = code_len(c->bits);
            if (n <= bits) continue;
            uint32_t pre = code_bits(c->bits) >> (n - bits);
            if (n - bits > sub_need[pre]) sub_need[pre] = n - bits;
        }
    int total = size;
    for (int i = 0; i < size; i++) if (sub_need[i]) total += 1 << sub_need[i];
    int32_t *lut = malloc((size_t)total * sizeof *lut);
    if (!lut) { free(sub_need); return -ENOMEM; }
    for (int i = 0; i < total; i++) lut[i] = V_BAD << 5 | 1;
    int next = size;
    for (int i = 0; i < size; i++) {
        if (!sub_need[i]) continue;
        lut[i] = -(next << 5 | sub_need[i]);
        next += 1 << sub_need[i];
    }
    for (int l = 0; l < 2; l++)
        for (const struct vlc_code *c = lists[l]; c && c->bits; c++) {
            int n = code_len(c->bits);
            uint32_t code = code_bits(c->bits);
            if (n <= bits) {
                uint32_t first = code << (bits - n);
                for (uint32_t k = 0; k < (1u << (bits - n)); k++) lut[first + k] = c->val << 5 | n;
            } else {
                uint32_t pre = code >> (n - bits);
                int sb = -lut[pre] & 31, off = -lut[pre] >> 5;
                int rest = n - bits;
                uint32_t low = code & ((1u << rest) - 1);
                uint32_t first = low << (sb - rest);
                for (uint32_t k = 0; k < (1u << (sb - rest)); k++) lut[off + first + k] = c->val << 5 | rest;
            }
        }
    free(sub_need);
    v->lut = lut;
    v->bits = bits;
    return 0;
}

static struct vlc g_mbai, g_mbtype[5], g_cbp, g_motion, g_dc_luma, g_dc_chroma, g_dct_first, g_dct_next;
static int g_tables_rc;
// ONCE: a player's decoding thread and a thumbnailer may both be first.
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;

static const struct vlc_code T_DCT_NEXT_HEAD[] = { { "10", V_EOB }, { "11", RL(0, 1) }, END };
static const struct vlc_code T_DCT_FIRST_HEAD[] = { { "1", RL(0, 1) }, END };

static void build_tables_once(void) {
    int rc = 0;
    rc |= vlc_build(&g_mbai, 8, T_MBAI, 0);
    rc |= vlc_build(&g_mbtype[1], 6, T_MBTYPE_I, 0);
    rc |= vlc_build(&g_mbtype[2], 6, T_MBTYPE_P, 0);
    rc |= vlc_build(&g_mbtype[3], 6, T_MBTYPE_B, 0);
    rc |= vlc_build(&g_mbtype[4], 6, T_MBTYPE_D, 0);
    rc |= vlc_build(&g_cbp, 9, T_CBP, 0);
    rc |= vlc_build(&g_motion, 8, T_MOTION, 0);
    rc |= vlc_build(&g_dc_luma, 7, T_DC_LUMA, 0);
    rc |= vlc_build(&g_dc_chroma, 8, T_DC_CHROMA, 0);
    rc |= vlc_build(&g_dct_first, 8, T_DCT_FIRST_HEAD, T_DCT);
    rc |= vlc_build(&g_dct_next, 8, T_DCT_NEXT_HEAD, T_DCT);
    g_tables_rc = rc ? -ENOMEM : 0;
}

static int build_tables(void) {
    pthread_once(&g_tables_once, build_tables_once);
    return g_tables_rc;
}

// Is every table a PREFIX CODE -- no code the start of another? A
// misplaced bit in a hand-typed table breaks exactly this. 0 when all
// are; otherwise the number of clashes. Needs no video, so the guest
// test and the host check both run it first.
static int prefix_clashes(const struct vlc_code *a, const struct vlc_code *b) {
    const struct vlc_code *lists[2] = { a, b };
    int bad = 0;
    for (int la = 0; la < 2; la++)
        for (const struct vlc_code *x = lists[la]; x && x->bits; x++)
            for (int lb = 0; lb < 2; lb++)
                for (const struct vlc_code *y = lists[lb]; y && y->bits; y++) {
                    if (x == y) continue;
                    size_t nx = strlen(x->bits), ny = strlen(y->bits);
                    if (nx <= ny && !strncmp(x->bits, y->bits, nx)) bad++;
                }
    return bad;
}

int uvid_mpeg1_selftest(void) {
    return prefix_clashes(T_MBAI, 0) + prefix_clashes(T_MBTYPE_I, 0) + prefix_clashes(T_MBTYPE_P, 0) +
           prefix_clashes(T_MBTYPE_B, 0) + prefix_clashes(T_CBP, 0) + prefix_clashes(T_MOTION, 0) +
           prefix_clashes(T_DC_LUMA, 0) + prefix_clashes(T_DC_CHROMA, 0) +
           prefix_clashes(T_DCT_FIRST_HEAD, T_DCT) + prefix_clashes(T_DCT_NEXT_HEAD, T_DCT);
}

// --- reading bits -------------------------------------------------------

struct br {
    const uint8_t *p;       // PAD zero bytes follow p[len - 1]
    size_t len;
    size_t pos;             // in bits
};

static inline uint32_t peek(const struct br *b, int n) {
    size_t byte = b->pos >> 3;
    if (byte + 4 > b->len + PAD) return 0;
    uint32_t v = (uint32_t)b->p[byte] << 24 | (uint32_t)b->p[byte + 1] << 16 |
                 (uint32_t)b->p[byte + 2] << 8 | b->p[byte + 3];
    v <<= b->pos & 7;
    return v >> (32 - n);
}

static inline void skip(struct br *b, int n) { b->pos += (size_t)n; }

static inline uint32_t get(struct br *b, int n) {
    uint32_t v = peek(b, n);
    skip(b, n);
    return v;
}

static inline int overrun(const struct br *b) { return b->pos > b->len * 8; }

static inline int vlc_get(struct br *b, const struct vlc *v) {
    int32_t e = v->lut[peek(b, v->bits)];
    if (e < 0) {
        skip(b, v->bits);
        int sb = -e & 31, off = -e >> 5;
        e = v->lut[off + (int)peek(b, sb)];
    }
    skip(b, e & 31);
    return e >> 5;
}

// --- the inverse DCT ---------------------------------------------------
//
// libjpeg's accurate integer IDCT (jidctint.c, the Loeffler-Ligtenberg-
// Moschytz factorisation in 13-bit fixed point): accurate enough for
// IEEE 1180, which is what MPEG asks of an IDCT. Columns, then rows;
// the output is the residual or the intra block's pixels before clamping.

#define CB 13
#define P1 2
#define FIX(x) ((int32_t)((x) * 8192 + 0.5))
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))

static void idct(const int16_t *in, int32_t *out) {
    int32_t ws[64];
    for (int c = 0; c < 8; c++) {
        const int16_t *i = in + c;
        int32_t *w = ws + c;
        if (!i[8] && !i[16] && !i[24] && !i[32] && !i[40] && !i[48] && !i[56]) {
            int32_t dc = i[0] * (1 << P1);
            for (int k = 0; k < 8; k++) w[k * 8] = dc;
            continue;
        }
        int32_t z2 = i[16], z3 = i[48];
        int32_t z1 = (z2 + z3) * FIX(0.541196100);
        int32_t t2 = z1 + z3 * -FIX(1.847759065), t3 = z1 + z2 * FIX(0.765366865);
        z2 = i[0]; z3 = i[32];
        int32_t t0 = (z2 + z3) * (1 << CB), t1 = (z2 - z3) * (1 << CB);
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = i[56]; t1 = i[40]; t2 = i[24]; t3 = i[8];
        z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2;
        int32_t z4 = t1 + t3, z5 = (z3 + z4) * FIX(1.175875602);
        t0 *= FIX(0.298631336); t1 *= FIX(2.053119869); t2 *= FIX(3.072711026); t3 *= FIX(1.501321110);
        z1 *= -FIX(0.899976223); z2 *= -FIX(2.562915447); z3 *= -FIX(1.961570560); z4 *= -FIX(0.390180644);
        z3 += z5; z4 += z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        w[0] = DESCALE(t10 + t3, CB - P1);  w[56] = DESCALE(t10 - t3, CB - P1);
        w[8] = DESCALE(t11 + t2, CB - P1);  w[48] = DESCALE(t11 - t2, CB - P1);
        w[16] = DESCALE(t12 + t1, CB - P1); w[40] = DESCALE(t12 - t1, CB - P1);
        w[24] = DESCALE(t13 + t0, CB - P1); w[32] = DESCALE(t13 - t0, CB - P1);
    }
    for (int r = 0; r < 8; r++) {
        const int32_t *w = ws + r * 8;
        int32_t *o = out + r * 8;
        int32_t z2 = w[2], z3 = w[6];
        int32_t z1 = (z2 + z3) * FIX(0.541196100);
        int32_t t2 = z1 + z3 * -FIX(1.847759065), t3 = z1 + z2 * FIX(0.765366865);
        int32_t t0 = (w[0] + w[4]) * (1 << CB), t1 = (w[0] - w[4]) * (1 << CB);
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = w[7]; t1 = w[5]; t2 = w[3]; t3 = w[1];
        z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2;
        int32_t z4 = t1 + t3, z5 = (z3 + z4) * FIX(1.175875602);
        t0 *= FIX(0.298631336); t1 *= FIX(2.053119869); t2 *= FIX(3.072711026); t3 *= FIX(1.501321110);
        z1 *= -FIX(0.899976223); z2 *= -FIX(2.562915447); z3 *= -FIX(1.961570560); z4 *= -FIX(0.390180644);
        z3 += z5; z4 += z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        int sh = CB + P1 + 3;
        o[0] = DESCALE(t10 + t3, sh); o[7] = DESCALE(t10 - t3, sh);
        o[1] = DESCALE(t11 + t2, sh); o[6] = DESCALE(t11 - t2, sh);
        o[2] = DESCALE(t12 + t1, sh); o[5] = DESCALE(t12 - t1, sh);
        o[3] = DESCALE(t13 + t0, sh); o[4] = DESCALE(t13 - t0, sh);
    }
}

static inline uint8_t clamp8(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// --- the decoder's state ---------------------------------------------------

static const uint8_t ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t DEFAULT_INTRA[64] = {
    8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

struct pic {
    uint8_t *y, *cb, *cr;
    int64_t pts;            // -1 when the stream gave none
    char type;
    int tref;               // its temporal reference: display order within its GOP
    uint32_t gop;           // which GOP, for the GOP's time base
};

// One ES packet's place in the byte stream, for giving pictures times.
struct pts_mark { uint64_t at; int64_t pts; int used; };
#define MARKS 64

struct m1 {
    // The elementary stream, gathered from packets. `base` is the stream
    // offset of es[0]; `scan` is where parsing resumes.
    uint8_t *es;
    size_t len, cap, scan;
    uint64_t base;
    struct pts_mark marks[MARKS];
    int nmarks;

    // From the sequence header.
    int w, h, mbw, mbh, ys, cs;
    uint32_t fps_num, fps_den;
    uint8_t qi[64], qn[64];         // intra and non-intra matrices, natural order
    int have_seq;

    struct pic pics[3];
    struct pic *fwd, *bwd;          // the anchors: earlier and later
    int bwd_shown;                  // the later anchor has been handed out
    struct uvid_frame out;
    int out_ready;
    int64_t last_pts;
    // A GOP's TIME BASE: the time its display-order picture 0 would be
    // shown, from any picture of it that carried a PTS. A packet's PTS
    // is the first picture STARTING in it, so most pictures carry none;
    // they are dated by temporal reference against the base (two GOPs
    // kept: an anchor is shown after the next GOP has begun).
    uint32_t gop;
    int64_t gop_base[2];
    int failed;                     // -ENOTSUP once a stream is found to be MPEG-2

    // Per picture.
    int type, full_fwd, full_bwd, r_fwd, r_bwd;
    struct pic *cur;
    // Per slice.
    int qs, dc_pred[3], mv_fwd[2], mv_bwd[2];
    int last_flags;                 // a B picture's skipped macroblocks repeat these
    int16_t blk[64];
    int32_t res[64];
    uint8_t pred[384], pred2[384];  // a macroblock's prediction: 16x16 Y, 8x8 Cb, 8x8 Cr
};

static void free_pics(struct m1 *m) {
    for (int i = 0; i < 3; i++) {
        free(m->pics[i].y);
        m->pics[i].y = m->pics[i].cb = m->pics[i].cr = 0;
    }
    m->fwd = m->bwd = 0;
}

static int alloc_pics(struct m1 *m) {
    free_pics(m);
    size_t ysz = (size_t)m->ys * (size_t)(m->mbh * 16), csz = (size_t)m->cs * (size_t)(m->mbh * 8);
    for (int i = 0; i < 3; i++) {
        uint8_t *p = malloc(ysz + 2 * csz);
        if (!p) { free_pics(m); return -ENOMEM; }
        memset(p, 16, ysz);
        memset(p + ysz, 128, 2 * csz);
        m->pics[i].y = p;
        m->pics[i].cb = p + ysz;
        m->pics[i].cr = p + ysz + csz;
    }
    return 0;
}

// --- headers ------------------------------------------------------------------

static const uint32_t FPS[9][2] = {
    { 0, 0 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 },
    { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 },
};

static int sequence_header(struct m1 *m, struct br *b) {
    int w = (int)get(b, 12), h = (int)get(b, 12);
    get(b, 4);                          // pel aspect ratio
    int rate = (int)get(b, 4);
    get(b, 18); get(b, 1); get(b, 10); get(b, 1);   // bit rate, marker, VBV size, constrained
    if (get(b, 1)) for (int i = 0; i < 64; i++) m->qi[ZIGZAG[i]] = (uint8_t)get(b, 8);
    else memcpy(m->qi, DEFAULT_INTRA, 64);
    if (get(b, 1)) for (int i = 0; i < 64; i++) m->qn[ZIGZAG[i]] = (uint8_t)get(b, 8);
    else memset(m->qn, 16, 64);
    for (int i = 0; i < 64; i++) {      // a zero weight is illegal; keep it harmless
        if (!m->qi[i]) m->qi[i] = 1;
        if (!m->qn[i]) m->qn[i] = 1;
    }
    if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) {
        uvid_fail("MPEG-1 picture size is out of range");
        return -ENOTSUP;
    }
    if (rate >= 1 && rate <= 8) { m->fps_num = FPS[rate][0]; m->fps_den = FPS[rate][1]; }
    if (!m->have_seq || w != m->w || h != m->h) {
        m->w = w;
        m->h = h;
        m->mbw = (w + 15) / 16;
        m->mbh = (h + 15) / 16;
        m->ys = m->mbw * 16;
        m->cs = m->mbw * 8;
        int rc = alloc_pics(m);
        if (rc) return rc;
    }
    m->have_seq = 1;
    return 0;
}

// --- motion ---------------------------------------------------------------

static int motion(struct br *b, int r_size, int pred) {
    int mag = vlc_get(b, &g_motion);
    if (mag == V_BAD) return pred;
    int code = mag && get(b, 1) ? -mag : mag;
    int f = 1 << r_size, d;
    if (f == 1 || code == 0) {
        d = code;
    } else {
        int r = (int)get(b, r_size);
        d = ((code < 0 ? -code : code) - 1) * f + r + 1;
        if (code < 0) d = -d;
    }
    int v = pred + d;
    if (v > 16 * f - 1) v -= 32 * f;
    else if (v < -16 * f) v += 32 * f;
    return v;
}

// One w x h block of `plane` (stride `st`, `pw` x `ph`) at (x, y) plus a
// half-pixel offset, into `dst` (stride `w`). Positions off the plane
// read its nearest edge -- legal streams never ask, damaged ones may.
static void predict_block(const uint8_t *plane, int st, int pw, int ph, int x, int y, int hx, int hy,
                          uint8_t *dst, int w, int h) {
    int inside = x >= 0 && y >= 0 && x + w + hx <= pw && y + h + hy <= ph;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int a, bb, c, d;
            if (inside) {
                const uint8_t *s = plane + (size_t)(y + j) * (size_t)st + (size_t)(x + i);
                a = s[0]; bb = s[hx]; c = s[hy * st]; d = s[hy * st + hx];
            } else {
                int x0 = x + i, y0 = y + j, x1 = x0 + hx, y1 = y0 + hy;
                x0 = x0 < 0 ? 0 : x0 >= pw ? pw - 1 : x0;
                x1 = x1 < 0 ? 0 : x1 >= pw ? pw - 1 : x1;
                y0 = y0 < 0 ? 0 : y0 >= ph ? ph - 1 : y0;
                y1 = y1 < 0 ? 0 : y1 >= ph ? ph - 1 : y1;
                a = plane[(size_t)y0 * st + x0]; bb = plane[(size_t)y0 * st + x1];
                c = plane[(size_t)y1 * st + x0]; d = plane[(size_t)y1 * st + x1];
            }
            int v;
            if (hx && hy) v = (a + bb + c + d + 2) >> 2;
            else if (hx) v = (a + bb + 1) >> 1;
            else if (hy) v = (a + c + 1) >> 1;
            else v = a;
            dst[j * w + i] = (uint8_t)v;
        }
    }
}

// The whole macroblock's prediction from `ref` along a half-pel vector.
static void predict_mb(const struct m1 *m, const struct pic *ref, int mbx, int mby, int vx, int vy, uint8_t *out) {
    int lx = mbx * 16 + (vx >> 1), ly = mby * 16 + (vy >> 1);
    predict_block(ref->y, m->ys, m->ys, m->mbh * 16, lx, ly, vx & 1, vy & 1, out, 16, 16);
    int cvx = vx / 2, cvy = vy / 2;     // truncating: the standard's own rule
    int cx = mbx * 8 + (cvx >> 1), cy = mby * 8 + (cvy >> 1);
    predict_block(ref->cb, m->cs, m->cs, m->mbh * 8, cx, cy, cvx & 1, cvy & 1, out + 256, 8, 8);
    predict_block(ref->cr, m->cs, m->cs, m->mbh * 8, cx, cy, cvx & 1, cvy & 1, out + 320, 8, 8);
}

// --- blocks -------------------------------------------------------------------

// Reads one block's coefficients, dequantised, into m->blk. 0, or -1 on
// a stream this cannot be.
static int read_block(struct m1 *m, struct br *b, int bi, int intra) {
    int16_t *blk = m->blk;
    memset(blk, 0, sizeof m->blk);
    int n = 0;
    const struct vlc *t = &g_dct_first;
    if (intra) {
        int size = vlc_get(b, bi < 4 ? &g_dc_luma : &g_dc_chroma);
        if (size == V_BAD) return -1;
        int diff = 0;
        if (size) {
            diff = (int)get(b, size);
            if (!(diff & (1 << (size - 1)))) diff -= (1 << size) - 1;
        }
        int *pred = &m->dc_pred[bi < 4 ? 0 : bi - 3];
        *pred += diff;
        blk[0] = (int16_t)(*pred * 8);
        n = 1;
        t = &g_dct_next;
    }
    const uint8_t *q = intra ? m->qi : m->qn;
    for (;;) {
        int v = vlc_get(b, t);
        t = &g_dct_next;
        int run, level;
        if (v == V_EOB) break;
        if (v == V_BAD) return -1;
        if (v == V_ESCAPE) {
            run = (int)get(b, 6);
            level = (int)get(b, 8);
            if (level == 0) level = (int)get(b, 8);
            else if (level == 128) level = (int)get(b, 8) - 256;
            else if (level > 128) level -= 256;
        } else {
            run = v >> 8;
            level = v & 255;
            if (get(b, 1)) level = -level;
        }
        n += run;
        if (n > 63) return -1;
        int z = ZIGZAG[n];
        int sign = level < 0 ? -1 : 1;
        int rec = intra ? (2 * level * m->qs * q[z]) / 16
                        : ((2 * level + sign) * m->qs * q[z]) / 16;
        if (rec && !(rec & 1)) rec -= rec > 0 ? 1 : -1;   // "oddification", against IDCT mismatch
        if (rec > 2047) rec = 2047;
        if (rec < -2048) rec = -2048;
        blk[z] = (int16_t)rec;
        n++;
        if (overrun(b)) return -1;
    }
    return 0;
}

// Block `bi` of macroblock (mbx, mby) in the current picture: the pixels
// (intra) or the prediction plus the residual.
static void put_block(struct m1 *m, int bi, int mbx, int mby, const uint8_t *pred, int has_res) {
    uint8_t *dst;
    int st, ps;
    const uint8_t *p;
    if (bi < 4) {
        st = m->ys;
        dst = m->cur->y + (size_t)(mby * 16 + (bi >> 1) * 8) * (size_t)st + (size_t)(mbx * 16 + (bi & 1) * 8);
        p = pred ? pred + (bi >> 1) * 8 * 16 + (bi & 1) * 8 : 0;
        ps = 16;
    } else {
        st = m->cs;
        dst = (bi == 4 ? m->cur->cb : m->cur->cr) + (size_t)(mby * 8) * (size_t)st + (size_t)(mbx * 8);
        p = pred ? pred + (bi == 4 ? 256 : 320) : 0;
        ps = 8;
    }
    if (has_res) idct(m->blk, m->res);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int v = (p ? p[y * ps + x] : 0) + (has_res ? m->res[y * 8 + x] : 0);
            dst[(size_t)y * (size_t)st + (size_t)x] = clamp8(v);
        }
}

// --- macroblocks and slices ----------------------------------------------

static void reset_dc(struct m1 *m) { m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = 128; }

// A predicted macroblock with this flag set, these vectors, and coded
// blocks `cbp` (0 for a skipped one).
static int inter_mb(struct m1 *m, struct br *b, int mbx, int mby, int flags, int cbp) {
    int fx = m->mv_fwd[0] << m->full_fwd, fy = m->mv_fwd[1] << m->full_fwd;
    int bx = m->mv_bwd[0] << m->full_bwd, by = m->mv_bwd[1] << m->full_bwd;
    const uint8_t *pred;
    if (m->type == 2) {
        predict_mb(m, m->bwd, mbx, mby, fx, fy, m->pred);   // P: the previous anchor
        pred = m->pred;
    } else if ((flags & MB_FWD) && (flags & MB_BWD)) {
        predict_mb(m, m->fwd, mbx, mby, fx, fy, m->pred);
        predict_mb(m, m->bwd, mbx, mby, bx, by, m->pred2);
        for (int i = 0; i < 384; i++) m->pred[i] = (uint8_t)((m->pred[i] + m->pred2[i] + 1) >> 1);
        pred = m->pred;
    } else if (flags & MB_BWD) {
        predict_mb(m, m->bwd, mbx, mby, bx, by, m->pred);
        pred = m->pred;
    } else {
        predict_mb(m, m->fwd, mbx, mby, fx, fy, m->pred);
        pred = m->pred;
    }
    for (int bi = 0; bi < 6; bi++) {
        int coded = (cbp >> (5 - bi)) & 1;
        if (coded && read_block(m, b, bi, 0) != 0) return -1;
        put_block(m, bi, mbx, mby, pred, coded);
    }
    return 0;
}

static int slice(struct m1 *m, struct br *b, int row) {
    int count = m->mbw * m->mbh;
    m->qs = (int)get(b, 5);
    if (!m->qs) return -1;
    while (get(b, 1)) get(b, 8);
    reset_dc(m);
    m->mv_fwd[0] = m->mv_fwd[1] = m->mv_bwd[0] = m->mv_bwd[1] = 0;
    m->last_flags = MB_FWD;
    int addr = row * m->mbw - 1, first = 1;
    while (peek(b, 23) != 0 && !overrun(b)) {
        int inc = 0;
        for (;;) {
            int v = vlc_get(b, &g_mbai);
            if (v == V_STUFF) continue;
            if (v == V_ESCAPE) { inc += 33; continue; }
            if (v == V_BAD) return -1;
            inc += v;
            break;
        }
        if (first) {
            addr += inc;
            first = 0;
        } else {
            // Skipped macroblocks: P copies from the anchor, B repeats
            // the last macroblock's prediction; I may not skip.
            for (int k = 1; k < inc; k++) {
                int a = addr + k;
                if (a >= count || m->type == 1 || m->type == 4) return -1;
                reset_dc(m);
                if (m->type == 2) { m->mv_fwd[0] = m->mv_fwd[1] = 0; }
                if (inter_mb(m, b, a % m->mbw, a / m->mbw, m->type == 2 ? MB_FWD : m->last_flags, 0)) return -1;
            }
            addr += inc;
        }
        if (addr < 0 || addr >= count) return -1;
        int mbx = addr % m->mbw, mby = addr / m->mbw;

        int flags = vlc_get(b, &g_mbtype[m->type]);
        if (flags == V_BAD) return -1;
        if (flags & MB_QUANT) {
            m->qs = (int)get(b, 5);
            if (!m->qs) return -1;
        }
        if (flags & MB_INTRA) {
            m->mv_fwd[0] = m->mv_fwd[1] = m->mv_bwd[0] = m->mv_bwd[1] = 0;
            for (int bi = 0; bi < 6; bi++) {
                if (read_block(m, b, bi, 1) != 0) return -1;
                put_block(m, bi, mbx, mby, 0, 1);
            }
            if (m->type == 4) get(b, 1);            // end_of_macroblock
            continue;
        }
        reset_dc(m);
        if (flags & MB_FWD) {
            m->mv_fwd[0] = motion(b, m->r_fwd, m->mv_fwd[0]);
            m->mv_fwd[1] = motion(b, m->r_fwd, m->mv_fwd[1]);
        } else if (m->type == 2) {
            m->mv_fwd[0] = m->mv_fwd[1] = 0;        // P with no vector: a zero one
        }
        if (flags & MB_BWD) {
            m->mv_bwd[0] = motion(b, m->r_bwd, m->mv_bwd[0]);
            m->mv_bwd[1] = motion(b, m->r_bwd, m->mv_bwd[1]);
        }
        int cbp = 0;
        if (flags & MB_PATTERN) {
            cbp = vlc_get(b, &g_cbp);
            if (cbp == V_BAD) return -1;
        }
        if (m->type == 3) m->last_flags = flags & (MB_FWD | MB_BWD);
        if (inter_mb(m, b, mbx, mby, flags, cbp)) return -1;
    }
    return 0;
}

// --- pictures -------------------------------------------------------------

static int64_t pts_for(struct m1 *m, uint64_t at) {
    int best = -1;
    for (int i = 0; i < m->nmarks; i++)
        if (m->marks[i].at <= at && (best < 0 || m->marks[i].at >= m->marks[best].at)) best = i;
    if (best < 0 || m->marks[best].used || m->marks[best].pts < 0) return -1;
    m->marks[best].used = 1;
    return m->marks[best].pts;
}

static int64_t tref_ms(const struct m1 *m, int tref) {
    return m->fps_num ? (int64_t)tref * 1000 * m->fps_den / m->fps_num : (int64_t)tref * 40;
}

static void emit(struct m1 *m, struct pic *p) {
    int64_t dur = m->fps_num ? (int64_t)1000 * m->fps_den / m->fps_num : 40;
    int64_t t = p->pts;
    if (t < 0 && m->gop - p->gop < 2 && m->gop_base[p->gop & 1] >= 0)
        t = m->gop_base[p->gop & 1] + tref_ms(m, p->tref);
    if (t < 0 && m->last_pts >= 0) t = m->last_pts + dur;
    // Untimed and nothing to count from (just after a seek): not shown,
    // since a wrong time is worse than a missing frame to an exact seek.
    if (t < 0) return;
    m->last_pts = t;
    m->out.w = m->w;
    m->out.h = m->h;
    m->out.fmt = UVID_YUV420;
    m->out.y = p->y;
    m->out.cb = p->cb;
    m->out.cr = p->cr;
    m->out.y_stride = m->ys;
    m->out.c_stride = m->cs;
    m->out.full_range = 0;
    m->out.pts_ms = t;
    m->out.type = p->type;
    m->out_ready = 1;
}

// One picture: `d` from its picture start code to just before the next
// non-slice start code (or the end of the stream).
static void picture(struct m1 *m, const uint8_t *d, size_t n, uint64_t at) {
    if (!m->have_seq) return;
    struct br b = { d + 4, n - 4, 0 };
    int tref = (int)get(&b, 10);
    int type = (int)get(&b, 3);
    get(&b, 16);                            // vbv delay
    if (type < 1 || type > 4) return;
    m->full_fwd = m->full_bwd = 0;
    m->r_fwd = m->r_bwd = 0;
    if (type == 2 || type == 3) {
        m->full_fwd = (int)get(&b, 1);
        int f = (int)get(&b, 3);
        if (!f) return;
        m->r_fwd = f - 1;
    }
    if (type == 3) {
        m->full_bwd = (int)get(&b, 1);
        int f = (int)get(&b, 3);
        if (!f) return;
        m->r_bwd = f - 1;
    }
    int64_t pts = pts_for(m, at);
    if (pts >= 0) m->gop_base[m->gop & 1] = pts - tref_ms(m, tref);

    // A picture whose references are missing (after a seek, before the
    // next I) cannot be decoded; it is dropped, as every decoder does.
    if (type == 3 && (!m->fwd || !m->bwd)) return;
    if (type == 2 && !m->bwd) return;

    m->type = type;
    struct pic *cur;
    if (type == 3) {
        cur = &m->pics[0];
        while (cur == m->fwd || cur == m->bwd) cur++;
    } else {
        cur = m->fwd ? m->fwd : &m->pics[0];
        while (!m->fwd && cur == m->bwd) cur++;
    }
    m->cur = cur;
    cur->pts = pts;
    cur->tref = tref;
    cur->gop = m->gop;
    cur->type = type == 1 ? 'I' : type == 2 ? 'P' : type == 3 ? 'B' : 'D';

    // Every slice: a start code 01..AF, its number the macroblock row + 1.
    for (size_t i = 4; i + 4 <= n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        int code = d[i + 3];
        if (code < 1 || code > 0xAF) continue;
        struct br sb = { d + i + 4, n - i - 4, 0 };
        if (code - 1 < m->mbh) slice(m, &sb, code - 1);
        i += 3;
    }

    if (type == 3) {
        emit(m, cur);
    } else {
        struct pic *prev = m->bwd;
        int prev_shown = m->bwd_shown;
        m->fwd = m->bwd;
        m->bwd = cur;
        m->bwd_shown = 0;
        if (prev && !prev_shown) emit(m, prev);
    }
}

// --- the codec --------------------------------------------------------------

static int m1_open(struct uvid *v) {
    if (build_tables() != 0) { uvid_fail("out of memory for the MPEG-1 tables"); return -ENOMEM; }
    struct m1 *m = calloc(1, sizeof *m);
    if (!m) return -ENOMEM;
    m->last_pts = -1;
    m->gop_base[0] = m->gop_base[1] = -1;
    v->cpriv = m;
    return 0;
}

static int m1_feed(struct uvid *v, const struct uvid_pkt *p) {
    struct m1 *m = v->cpriv;
    if (m->len + p->len + PAD > m->cap) {
        size_t cap = m->cap ? m->cap : 256 * 1024;
        while (cap < m->len + p->len + PAD) cap *= 2;
        uint8_t *es = realloc(m->es, cap);
        if (!es) { uvid_fail("out of memory for the video stream"); return -ENOMEM; }
        m->es = es;
        m->cap = cap;
    }
    if (p->pts_ms >= 0) {
        if (m->nmarks == MARKS) { memmove(m->marks, m->marks + 1, sizeof m->marks[0] * (MARKS - 1)); m->nmarks--; }
        m->marks[m->nmarks].at = m->base + m->len;
        m->marks[m->nmarks].pts = p->pts_ms;
        m->marks[m->nmarks].used = 0;
        m->nmarks++;
    }
    memcpy(m->es + m->len, p->data, p->len);
    m->len += p->len;
    memset(m->es + m->len, 0, PAD);
    return 0;
}

// The next start code at or after `from`, or `len` when there is none.
static size_t next_start(const struct m1 *m, size_t from) {
    for (size_t i = from; i + 3 < m->len; i++)
        if (!m->es[i] && !m->es[i + 1] && m->es[i + 2] == 1) return i;
    return m->len;
}

// Parses units until a frame comes out or the buffered stream runs dry.
static int m1_frame(struct uvid *v, int eof, const struct uvid_frame **out) {
    struct m1 *m = v->cpriv;
    if (m->failed) return 0;
    for (;;) {
        if (m->out_ready) {
            m->out_ready = 0;
            *out = &m->out;
            return 1;
        }
        size_t s = next_start(m, m->scan);
        if (s >= m->len) {
            if (eof && m->bwd && !m->bwd_shown) {   // the last anchor, held back until now
                emit(m, m->bwd);
                m->bwd_shown = 1;
                continue;
            }
            m->scan = m->len > 3 ? m->len - 3 : 0;
            break;
        }
        int code = m->es[s + 3];
        // Where this unit ends: the next start code that is not a slice.
        size_t e = s + 4;
        for (;;) {
            e = next_start(m, e);
            if (e >= m->len || code != 0 || m->es[e + 3] < 1 || m->es[e + 3] > 0xAF) break;
            e += 4;
        }
        if (e >= m->len && !eof) { m->scan = s; break; }   // the unit is not all here yet
        struct br b = { m->es + s + 4, e - s - 4, 0 };
        if (code == 0xB3) {
            if (sequence_header(m, &b) != 0) { m->failed = 1; return 0; }
        } else if (code == 0xB5) {
            m->failed = 1;                  // a sequence extension: MPEG-2
            uvid_fail("MPEG-2 video is not supported");
            return 0;
        } else if (code == 0xB8) {
            m->gop++;                       // a new GOP: its base is not known yet
            m->gop_base[m->gop & 1] = -1;
        } else if (code == 0x00) {
            picture(m, m->es + s, e - s, m->base + s);
        }
        m->scan = e > s + 4 ? e : s + 4;
        if (m->scan >= m->len) m->scan = m->len;
    }
    // Keep the buffer small: drop what has been parsed.
    if (m->scan > 0) {
        size_t keep = m->len - m->scan;
        memmove(m->es, m->es + m->scan, keep);
        m->base += m->scan;
        m->len = keep;
        m->scan = 0;
        memset(m->es + m->len, 0, PAD);
    }
    return 0;
}

static void m1_reset(struct uvid *v) {
    struct m1 *m = v->cpriv;
    m->len = m->scan = 0;
    m->nmarks = 0;
    m->fwd = m->bwd = 0;
    m->bwd_shown = 0;
    m->out_ready = 0;
    m->last_pts = -1;
    m->gop_base[0] = m->gop_base[1] = -1;
}

static void m1_close(struct uvid *v) {
    struct m1 *m = v->cpriv;
    if (!m) return;
    free_pics(m);
    free(m->es);
    free(m);
    v->cpriv = 0;
}

const struct uvid_codec uvid_codec_mpeg1 = {
    .id = UVID_VC_MPEG1,
    .name = "mpeg1",
    .open = m1_open,
    .feed = m1_feed,
    .frame = m1_frame,
    .reset = m1_reset,
    .close = m1_close,
};
