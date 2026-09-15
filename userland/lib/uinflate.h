#ifndef ULIB_UINFLATE_H
#define ULIB_UINFLATE_H

#include <stdint.h>
#include <stddef.h>

// DEFLATE, both directions, in ring 3 -- RFC 1951, plus the zlib (RFC
// 1950) and gzip (RFC 1952) wrappers around it.
//
// Two real callers: `uimg_png.c` reads and writes zlib streams, and
// `/bin/wget` inflates a gzip Content-Encoding.
//
// **RING 3, AND THE KERNEL DOES NOT NEED A COPY -- MEASURED.** This
// briefly lived in `kernel/lib/` so the kernel could unpack the
// compressed live image, and the code never ran: GRUB decompresses any
// file whose CONTENT begins with the gzip magic, the module and the
// kernel image alike. Linux carries `lib/zlib_inflate/` because its
// loader does NOT do that for an initramfs; this one does, so there is
// no ring-0 caller to compile twice for (docs/decisions/build.md).
//
// **IT ALLOCATES NOTHING**, which is worth keeping even back in ring 3.
// The 32 KiB history window is a caller-supplied `struct
// uinflate_scratch` -- api/ttf.h's shape -- so a caller decides where it
// lives and nothing here hides an allocation inside a decode.
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
#define UINFLATE_WBITS 15
#define UINFLATE_WSIZE (1 << UINFLATE_WBITS)

// The 4 KiB is the chunk handed to the output callback. It is HERE
// rather than on the stack because `struct sink` holding it put a 6.5 KB
// frame in uinflate(), three times the ring-3 budget -- and a caller who
// has already been asked for a scratch should not then be surprised by a
// second buffer hidden in a frame.
#define UINFLATE_CHUNK 4096

// The two Huffman tables live here for the same reason as the chunk:
// they are ~600 bytes each and a block loop holding both put
// inflate_blocks() over the frame budget. Opaque to a caller -- sized in
// bytes so this header does not have to publish the decoder's internals.
#define UINFLATE_TABLES 1280

struct uinflate_scratch {
    unsigned char window[UINFLATE_WSIZE];
    unsigned char chunk[UINFLATE_CHUNK];
    unsigned char tables[UINFLATE_TABLES];
};

// Which wrapper is around the deflate stream.
enum uinflate_wrap {
    UINFLATE_RAW,   // bare RFC 1951
    UINFLATE_ZLIB,  // RFC 1950: 2-byte header, adler32 trailer (PNG)
    UINFLATE_GZIP,  // RFC 1952: magic + flags, crc32 and size trailer
};

// Receives decompressed bytes in order. Return 0 to continue, non-zero
// to stop -- which kinflate() reports as -EIO, so a sink that runs out
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
//   -ENOSPC   `uinflate_into` was given a buffer smaller than the
//             stream decompresses to. Refused, never truncated.
//
// uinflate_error() carries the sentence, the same contract uimg.h uses.
int uinflate(const void *src, size_t n, enum uinflate_wrap wrap,
             struct uinflate_scratch *scratch,
             uinflate_out out, void *ctx, size_t *out_len);

// Into a buffer whose size the caller already knows -- PNG does, from
// its own header. REFUSES rather than truncating if the stream produces
// more than `cap`: a short image is a plausible-looking wrong picture,
// which is worse than an error (the rule kfmt's formatters follow).
int uinflate_into(const void *src, size_t n, enum uinflate_wrap wrap,
                  struct uinflate_scratch *scratch,
                  void *dst, size_t cap, size_t *out_len);

// THE LENGTH AND DISTANCE TABLES (RFC 1951 3.2.5), defined once here
// and used by the compressor too (lib/udeflate.h). Two copies is how an
// encoder and a decoder come to disagree about what a length code means
// -- a disagreement that produces a valid-looking file nothing can read.
extern const uint16_t uinflate_len_base[29];
extern const uint8_t  uinflate_len_extra[29];
extern const uint16_t uinflate_dist_base[30];
extern const uint8_t  uinflate_dist_extra[30];

// --- compressing ------------------------------------------------------
//
// **GREEDY LZ77 INTO ONE FIXED-HUFFMAN BLOCK.** No dynamic code table
// and no lazy matching: both are worth real ratio on text and close to
// nothing on the long flat runs this was written for (a screenshot's
// filtered rows). The DECODER reads dynamic blocks regardless, so what
// this emits never limits what can be read -- which is why the simpler
// encoder is not a corner being cut.

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
