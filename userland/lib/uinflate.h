#ifndef ULIB_UINFLATE_H
#define ULIB_UINFLATE_H

#include <stdint.h>
#include <stddef.h>

// DEFLATE, both directions, in ring 3 -- RFC 1951, plus the zlib (RFC
// 1950) and gzip (RFC 1952) wrappers around it.
//
// It is a library rather than something private to the PNG codec
// because it has two real callers: `uimg_png.c` reads and writes zlib
// streams, and `/bin/wget` inflates a gzip Content-Encoding. The
// compressor moved here out of uimg_png.c when the decompressor arrived
// beside it -- one module, both directions, one place to test.
//
// **RING 3 ONLY, AND THAT IS NOT AN OVERSIGHT.** Linux carries
// `lib/zlib_inflate/` in the kernel, but it is there to unpack the
// kernel image and the initrd; toy-os compresses neither, so there is no
// ring-0 caller and nothing to compile twice for. The day a compressed
// module or boot artifact exists, this file is a `kernel/lib/` move away
// -- it names nothing kernel-side and nothing libc-side beyond malloc.
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

// Which wrapper is around the deflate stream.
enum uinflate_wrap {
    UINFLATE_RAW,   // bare RFC 1951
    UINFLATE_ZLIB,  // RFC 1950: 2-byte header, adler32 trailer (PNG)
    UINFLATE_GZIP,  // RFC 1952: magic + flags, crc32 and size trailer
};

// Receives decompressed bytes in order. Return 0 to continue, non-zero
// to stop -- which uinflate() reports as -EIO, so a sink that runs out
// of room stops the decode instead of being called again forever.
typedef int (*uinflate_out)(void *ctx, const uint8_t *data, size_t n);

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
//   -ENOMEM   the window could not be allocated.
//
// uinflate_error() carries the sentence, the same contract uimg.h uses.
int uinflate(const void *src, size_t n, enum uinflate_wrap wrap,
             uinflate_out out, void *ctx, size_t *out_len);

// Into a buffer whose size the caller already knows -- PNG does, from
// its own header. REFUSES rather than truncating if the stream produces
// more than `cap`: a short image is a plausible-looking wrong picture,
// which is worse than an error (the rule kfmt's formatters follow).
int uinflate_into(const void *src, size_t n, enum uinflate_wrap wrap,
                  void *dst, size_t cap, size_t *out_len);

// Into a fresh allocation the caller frees. `limit` is a ceiling on the
// result, refused rather than truncated -- a decompression bomb is a
// 4 KiB file that expands to gigabytes, and every consumer of this has
// a size past which it should give up rather than a size it needs.
int uinflate_alloc(const void *src, size_t n, enum uinflate_wrap wrap,
                   uint8_t **out, size_t *out_len, size_t limit);

// --- compressing ------------------------------------------------------
//
// Greedy LZ77 into one fixed-Huffman block. No dynamic code table and no
// lazy matching: both are worth real ratio on text and close to nothing
// on the long flat runs this was written for (a screenshot's filtered
// rows), and the decoder above reads dynamic blocks regardless, so what
// this emits is never what limits what it can read.

// Compresses into `dst`, at most `cap` bytes, wrapping as asked.
// Returns 0, or -ENOMEM when the result did not fit (which for a
// pathological input can exceed the source) or the match tables could
// not be allocated.
int udeflate_into(const void *src, size_t n, enum uinflate_wrap wrap,
                  void *dst, size_t cap, size_t *out_len);

// A bound `cap` can safely be: deflate's worst case plus the wrapper.
size_t udeflate_bound(size_t n);

const char *uinflate_error(void);

#endif // ULIB_UINFLATE_H
