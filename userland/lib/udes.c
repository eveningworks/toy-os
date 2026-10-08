// See udes.h. The tables are FIPS 46-3's, transcribed with their
// 1-based bit numbers (bit 1 = the most significant of the 64), which
// keeps them checkable against the standard line by line.
#include "lib/udes.h"

static const uint8_t IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17,  9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const uint8_t FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41,  9, 49, 17, 57, 25 };
static const uint8_t E[48] = {
    32,  1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9, 10, 11,
    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32,  1 };
static const uint8_t P[32] = {
    16,  7, 20, 21, 29, 12, 28, 17,  1, 15, 23, 26,  5, 18, 31, 10,
     2,  8, 24, 14, 32, 27,  3,  9, 19, 13, 30,  6, 22, 11,  4, 25 };
static const uint8_t PC1[56] = {
    57, 49, 41, 33, 25, 17,  9,  1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27, 19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,  7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29, 21, 13,  5, 28, 20, 12,  4 };
static const uint8_t PC2[48] = {
    14, 17, 11, 24,  1,  5,  3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8, 16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const uint8_t SHIFTS[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const uint8_t S[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
      0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
      15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
      3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
      13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
      13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
      1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
      13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
      3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
      14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
      11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
      10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
      4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
      13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
      6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
      1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
      2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 } };

// Picks bits `tbl[0..n)` (1-based, out of `width`) of `v` into an n-bit
// result, first-listed most significant.
static uint64_t permute(uint64_t v, const uint8_t *tbl, int n, int width) {
    uint64_t r = 0;
    for (int i = 0; i < n; i++)
        r = (r << 1) | ((v >> (width - tbl[i])) & 1);
    return r;
}

static uint64_t load64(const uint8_t b[8]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

static void store64(uint64_t v, uint8_t b[8]) {
    for (int i = 7; i >= 0; i--) { b[i] = (uint8_t)v; v >>= 8; }
}

void udes_set_key(struct udes_key *k, const uint8_t key[8]) {
    uint64_t cd = permute(load64(key), PC1, 56, 64);
    uint32_t c = (uint32_t)(cd >> 28) & 0x0FFFFFFF, d = (uint32_t)cd & 0x0FFFFFFF;
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < SHIFTS[r]; s++) {
            c = ((c << 1) | (c >> 27)) & 0x0FFFFFFF;
            d = ((d << 1) | (d >> 27)) & 0x0FFFFFFF;
        }
        k->sub[r] = permute(((uint64_t)c << 28) | d, PC2, 48, 56);
    }
}

static uint32_t feistel(uint32_t r, uint64_t sub) {
    uint64_t x = permute(r, E, 48, 32) ^ sub;
    uint32_t out = 0;
    for (int i = 0; i < 8; i++) {
        unsigned six = (unsigned)(x >> (42 - 6 * i)) & 0x3F;
        // The outer two bits pick the row, the middle four the column.
        unsigned row = ((six & 0x20) >> 4) | (six & 1), col = (six >> 1) & 0xF;
        out = (out << 4) | S[i][row * 16 + col];
    }
    return (uint32_t)permute(out, P, 32, 32);
}

static void crypt_block(const struct udes_key *k, const uint8_t in[8], uint8_t out[8],
                        int decrypt) {
    uint64_t v = permute(load64(in), IP, 64, 64);
    uint32_t l = (uint32_t)(v >> 32), r = (uint32_t)v;
    for (int i = 0; i < 16; i++) {
        uint32_t t = r;
        r = l ^ feistel(r, k->sub[decrypt ? 15 - i : i]);
        l = t;
    }
    // The last round's halves are NOT swapped back: R16 L16 goes to FP.
    store64(permute(((uint64_t)r << 32) | l, FP, 64, 64), out);
}

void udes_encrypt(const struct udes_key *k, const uint8_t in[8], uint8_t out[8]) {
    crypt_block(k, in, out, 0);
}

void udes_decrypt(const struct udes_key *k, const uint8_t in[8], uint8_t out[8]) {
    crypt_block(k, in, out, 1);
}
