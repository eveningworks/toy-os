#ifndef KSHA256_H
#define KSHA256_H

#include <stddef.h>
#include <stdint.h>

// SHA-256 (FIPS 180-4), compiled into both rings from kernel/lib/ksha256.c
// -- the kernel debugger authenticates its network link with it, and
// /lib/libhash.so's `sum -a sha256` is the same code, checked against
// hashlib by tools/hash_hostcheck.py. Freestanding: no allocation.

#define KSHA256_LEN 32

struct ksha256 {
    uint32_t h[8];
    uint64_t len;          // bytes fed in, for the length pad
    unsigned char buf[64];
    unsigned n;            // bytes sitting in buf
};

void ksha256_init(struct ksha256 *c);
void ksha256_update(struct ksha256 *c, const void *data, size_t len);
void ksha256_final(struct ksha256 *c, unsigned char out[KSHA256_LEN]);

#endif
