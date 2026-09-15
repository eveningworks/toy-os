#ifndef KAPI_KINFLATE_H
#define KAPI_KINFLATE_H

#include <stdint.h>
#include <stddef.h>

// DECOMPRESSION -- RFC 1951 deflate, plus the zlib (RFC 1950) and gzip
// (RFC 1952) wrappers. COMPILED TWICE, ring 0 and ring 3.
//
// Three real callers, in two rings: `uimg_png.c` reads zlib streams,
// `/bin/wget` inflates a gzip Content-Encoding, and the kernel unpacks
// the compressed live filesystem image GRUB hands it as a module. That
// last one is why this is here rather than in `userland/lib/` -- it is
// the same reason Linux carries `lib/zlib_inflate/` in the kernel, which
// is to unpack its own initramfs.
//
// The COMPRESSOR is not here: it lives in `userland/lib/udeflate.h`,
// ring 3 only, because nothing in this kernel compresses anything and
// its match tables are 256 KB that ring 0 would carry for nobody.
//
// **IT ALLOCATES NOTHING.** The 32 KiB history window is a
// caller-supplied `struct kinflate_scratch`, which is what lets one
// implementation serve ring 0, ring 3 and a test -- the same shape
// api/ttf.h uses and for the same reason. A file on the shared-source
// path has the C library stripped from its include path, so it can name
// neither `malloc` nor `kmalloc`; a scratch is not a style choice here,
// it is the only thing that compiles.
//
// **THE INPUT IS ONE CONTIGUOUS BUFFER; THE OUTPUT IS A CALLBACK.**
// That asymmetry is the whole design and it is deliberate. A decompressor
// that can suspend anywhere -- mid-symbol, mid-match, mid-block -- is a
// state machine with a dozen resume points, and its bugs are the kind
// that only show up on an input split at an awkward byte. Consuming from
// one buffer keeps the decoder a straight loop; emitting through a
// callback means nothing has to know the decompressed size in advance.
//
// The cost, stated plainly: a caller holds the whole COMPRESSED input in
// memory. For PNG that is what it already had (the IDAT chunks are
// concatenated first either way), and for an HTTP body it is the
// compressed size -- which is the point of having asked for gzip.

// THE WORKING STATE, supplied by the caller. A back-reference can point
// 32 KiB behind the current byte, so that much history has to be kept
// whatever the output is doing with it.
//
// It is ~32 KiB, which is too big for any stack here (the kernel's frame
// budget is 1 KiB and ring 3's is 2 KiB): make it static, or allocate it
// where there is an allocator.
#define KINFLATE_WBITS 15
#define KINFLATE_WSIZE (1 << KINFLATE_WBITS)

struct kinflate_scratch {
    unsigned char window[KINFLATE_WSIZE];
};

// Which wrapper is around the deflate stream.
enum kinflate_wrap {
    KINFLATE_RAW,   // bare RFC 1951
    KINFLATE_ZLIB,  // RFC 1950: 2-byte header, adler32 trailer (PNG)
    KINFLATE_GZIP,  // RFC 1952: magic + flags, crc32 and size trailer
};

// Receives decompressed bytes in order. Return 0 to continue, non-zero
// to stop -- which kinflate() reports as -EIO, so a sink that runs out
// of room stops the decode instead of being called again forever.
typedef int (*kinflate_out)(void *ctx, const uint8_t *data, size_t n);

// The general form. `*out_len` gets the decompressed length, which is
// counted whether or not `out` is NULL -- so passing NULL measures.
//
// Returns 0, or a negative errno:
//   -EINVAL   malformed: a bad wrapper header, a reserved block type, a
//             distance pointing before the start of the stream, a
//             Huffman table that is not a complete prefix code, or a
//             checksum that does not match.
//   -ENOTSUP  a valid stream this build will not read (a gzip member
//             using a compression method other than deflate).
//   -EIO      the callback asked to stop.
//   -ENOSPC   `kinflate_into` was given a buffer smaller than the
//             stream decompresses to. Refused, never truncated.
//
// kinflate_error() carries the sentence, the same contract uimg.h uses.
int kinflate(const void *src, size_t n, enum kinflate_wrap wrap,
             struct kinflate_scratch *scratch,
             kinflate_out out, void *ctx, size_t *out_len);

// Into a buffer whose size the caller already knows -- PNG does, from
// its own header. REFUSES rather than truncating if the stream produces
// more than `cap`: a short image is a plausible-looking wrong picture,
// which is worse than an error (the rule kfmt's formatters follow).
int kinflate_into(const void *src, size_t n, enum kinflate_wrap wrap,
                  struct kinflate_scratch *scratch,
                  void *dst, size_t cap, size_t *out_len);

// THE LENGTH AND DISTANCE TABLES (RFC 1951 3.2.5), defined once here
// and used by the compressor too (lib/udeflate.h). Two copies is how an
// encoder and a decoder come to disagree about what a length code means
// -- a disagreement that produces a valid-looking file nothing can read.
extern const uint16_t kinflate_len_base[29];
extern const uint8_t  kinflate_len_extra[29];
extern const uint16_t kinflate_dist_base[30];
extern const uint8_t  kinflate_dist_extra[30];

const char *kinflate_error(void);

#endif // KAPI_KINFLATE_H
