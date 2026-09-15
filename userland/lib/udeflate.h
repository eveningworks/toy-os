#ifndef ULIB_UDEFLATE_H
#define ULIB_UDEFLATE_H

#include <stdint.h>
#include <stddef.h>
#include "kinflate.h"

// COMPRESSION -- RFC 1951 deflate, with the zlib and gzip wrappers.
// Ring 3 only, and the decompressor (api/kinflate.h) is not.
//
// The split is by WHO NEEDS IT rather than by symmetry. The kernel
// inflates the live filesystem image GRUB hands it, so the decompressor
// is on the shared-source path; nothing in the kernel ever compresses
// anything, and this side's match tables are 256 KB that ring 0 would
// carry for nobody. Keeping it here also lets it use `malloc`, which a
// shared-source file cannot name at all.
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
int udeflate_into(const void *src, size_t n, enum kinflate_wrap wrap,
                  void *dst, size_t cap, size_t *out_len);

// A bound `cap` can safely be: deflate's worst case plus the wrapper.
size_t udeflate_bound(size_t n);

const char *udeflate_error(void);

#endif // ULIB_UDEFLATE_H
