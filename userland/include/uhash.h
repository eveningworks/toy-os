#ifndef ULIB_UHASH_H
#define ULIB_UHASH_H

#include <stddef.h>
#include <stdint.h>

// Checksums and digests, as a TABLE rather than one function per
// algorithm. A third algorithm is a row in the table and a line in
// `sum`'s man page, not a new program.
//
// THIS IS /lib/libhash.so's PUBLIC HEADER, not the C library's -- it
// sits here because this is the directory every userland program can
// already reach, which is what makes the library usable by something
// nobody has written yet. The implementation is userland/dynlib/uhash.c
// and it is NOT in libuapp.a: a program that wants it links the shared
// object (ULIB_SO_<program> in the Makefile), so there is one copy of
// the code in the image and one in memory however many programs use it.
//
// That is coreutils 9.0's shape (`cksum -a sha256`), which replaced the
// md5sum/sha1sum/sha256sum family for the reason this project keeps
// legislating for C: a binary per algorithm multiplies. Nothing here is
// a MAC or a password hash -- SHA-256 is here to answer "did these bytes
// survive the trip", and toy-os has no crypto.

#define UHASH_DIGEST_MAX 32

// One algorithm's working state. A union, so a caller sizes it without
// knowing which row it picked.
union uhash_ctx {
    uint32_t crc;
    struct {
        uint32_t h[8];
        uint64_t len;         // total bytes fed in, for the length pad
        unsigned char buf[64];
        unsigned n;           // bytes sitting in buf
    } sha256;
};

struct uhash_alg {
    const char *name;
    unsigned digest_len;      // bytes written by final()

    // How the STANDARD tool for this algorithm prints a line, which is
    // what makes the host a usable oracle: 1 is cksum(1)'s
    // "<decimal> <bytes> <name>", 0 is sha256sum(1)'s "<hex>  <name>".
    // /bin/sum both writes and parses these, so a manifest moves in
    // either direction between this OS and a Linux box.
    int cksum_style;

    void (*init)(union uhash_ctx *);
    void (*update)(union uhash_ctx *, const void *data, size_t len);
    void (*final)(union uhash_ctx *, unsigned char *out);
};

// NULL if no algorithm has that name. Names are lower-case and exact --
// no aliases, because "crc32" meaning two different polynomials in two
// tools is the confusion this is trying not to add to.
const struct uhash_alg *uhash_find(const char *name);

// Walks the table for a usage message. NULL past the end.
const struct uhash_alg *uhash_nth(unsigned i);

// Lower-case hex of `len` bytes, NUL-terminated. `out` needs 2*len + 1.
void uhash_hex(const unsigned char *digest, unsigned len, char *out);

#endif // ULIB_UHASH_H
