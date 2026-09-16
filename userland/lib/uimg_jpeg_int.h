#ifndef UIMG_JPEG_INT_H
#define UIMG_JPEG_INT_H

#include <stdint.h>
#include <stddef.h>
#include "lib/uimg.h"

// What the JPEG DECODER and ENCODER share, and nothing else does.
//
// They live in two files because they are two concerns -- uimg_jpeg.c
// was 1249 lines before the encoder was written, and a reader looking
// for the IDCT should not be walking past a Huffman code table. What is
// genuinely common between them is small enough to list here: the
// zigzag order, and the error sink every failure path in both calls.
//
// NOT part of uimg.h. A caller reaches the encoder through
// uimg_encode(im, "jpeg", ...) like every other format; this header is
// the codec row's own wiring.

// Zigzag position -> index into a block in NATURAL (row-major) order.
// The decoder walks it to scatter coefficients, the encoder to gather
// them; it is the same permutation read in the same direction.
extern const uint8_t uimg_jpeg_zigzag[64];

// uimg.c owns the one-string-per-process error sink (uimg.h).
void uimg_set_error(const char *msg);

int uimg_jpeg_encode(const struct uimg *im, uint8_t **out, size_t *out_len);

#endif // UIMG_JPEG_INT_H
