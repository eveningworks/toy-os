// See uhash.h.
#include <uhash.h>
#include "kcrc.h"
#include <string.h>

// --- crc32 -----------------------------------------------------------
//
// The algorithm is kernel/lib/kcrc.c, compiled into both rings: a GPT
// header's CRC and `sum -a crc32` must agree, and two implementations
// of one polynomial is exactly how they would stop agreeing.

static void crc_init(union uhash_ctx *c) { c->crc = KCRC32_INIT; }

static void crc_update(union uhash_ctx *c, const void *data, size_t len) {
    c->crc = kcrc32_update(c->crc, data, len);
}

static void crc_final(union uhash_ctx *c, unsigned char *out) {
    uint32_t v = KCRC32_FINAL(c->crc);
    // BIG-ENDIAN, so the hex form reads the way every other tool prints
    // a CRC. The decimal form cksum uses reads it back as a number.
    out[0] = (unsigned char)(v >> 24);
    out[1] = (unsigned char)(v >> 16);
    out[2] = (unsigned char)(v >> 8);
    out[3] = (unsigned char)v;
}

// --- sha256 ----------------------------------------------------------
//
// FIPS 180-4. Plain, table-driven, no unrolling: this is bounded by the
// disk at every size that matters here.

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(union uhash_ctx *c, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->sha256.h[0], b = c->sha256.h[1], cc = c->sha256.h[2];
    uint32_t d = c->sha256.h[3], e = c->sha256.h[4], f = c->sha256.h[5];
    uint32_t g = c->sha256.h[6], h = c->sha256.h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->sha256.h[0] += a; c->sha256.h[1] += b; c->sha256.h[2] += cc;
    c->sha256.h[3] += d; c->sha256.h[4] += e; c->sha256.h[5] += f;
    c->sha256.h[6] += g; c->sha256.h[7] += h;
}

static void sha_init(union uhash_ctx *c) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for (int i = 0; i < 8; i++) c->sha256.h[i] = iv[i];
    c->sha256.len = 0;
    c->sha256.n = 0;
}

static void sha_update(union uhash_ctx *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    c->sha256.len += len;
    // Top up a partial block first; whole blocks then go straight
    // through without a copy.
    while (len > 0) {
        if (c->sha256.n == 0 && len >= 64) {
            sha_block(c, p);
            p += 64;
            len -= 64;
            continue;
        }
        unsigned room = 64 - c->sha256.n;
        unsigned take = len < room ? (unsigned)len : room;
        memcpy(c->sha256.buf + c->sha256.n, p, take);
        c->sha256.n += take;
        p += take;
        len -= take;
        if (c->sha256.n == 64) {
            sha_block(c, c->sha256.buf);
            c->sha256.n = 0;
        }
    }
}

static void sha_final(union uhash_ctx *c, unsigned char *out) {
    // 0x80, then zeroes, then the length in BITS as a big-endian 64.
    uint64_t bits = c->sha256.len * 8;
    unsigned char pad = 0x80;
    sha_update(c, &pad, 1);
    // `bits` was taken BEFORE the padding, which is the whole trick:
    // sha_update() keeps counting, so reading len here would include
    // the pad.
    static const unsigned char zeros[64] = { 0 };
    while (c->sha256.n != 56) {
        unsigned need = (c->sha256.n < 56) ? (56 - c->sha256.n) : (120 - c->sha256.n);
        sha_update(c, zeros, need < 64 ? need : 64);
    }
    unsigned char lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (unsigned char)(bits >> (56 - i * 8));
    sha_update(c, lenbuf, 8);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(c->sha256.h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->sha256.h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->sha256.h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->sha256.h[i]);
    }
}

// --- the table -------------------------------------------------------

static const struct uhash_alg g_algs[] = {
    { "crc32",  4,  1, crc_init, crc_update, crc_final },
    { "sha256", 32, 0, sha_init, sha_update, sha_final },
};

const struct uhash_alg *uhash_find(const char *name) {
    for (unsigned i = 0; i < sizeof g_algs / sizeof g_algs[0]; i++)
        if (strcmp(g_algs[i].name, name) == 0) return &g_algs[i];
    return NULL;
}

const struct uhash_alg *uhash_nth(unsigned i) {
    return i < sizeof g_algs / sizeof g_algs[0] ? &g_algs[i] : NULL;
}

void uhash_hex(const unsigned char *digest, unsigned len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < len; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[len * 2] = '\0';
}
