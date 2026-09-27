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
// kernel/lib/ksha256.c, compiled into both rings: the kernel debugger's
// link is authenticated with it, and one implementation is one to check.

static void sha_init(union uhash_ctx *c) { ksha256_init(&c->sha256); }

static void sha_update(union uhash_ctx *c, const void *data, size_t len) {
    ksha256_update(&c->sha256, data, len);
}

static void sha_final(union uhash_ctx *c, unsigned char *out) {
    ksha256_final(&c->sha256, out);
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
