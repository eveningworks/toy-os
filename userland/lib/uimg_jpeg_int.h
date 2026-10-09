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

// Annex K.3's standard Huffman tables, counts indexed 1..16 (entry 0 is
// unused): the encoder writes them, and the decoder falls back to them
// for a scan whose table the file never defined (Motion JPEG).
extern const uint8_t uimg_jpeg_std_dc_lum_bits[17], uimg_jpeg_std_dc_chr_bits[17];
extern const uint8_t uimg_jpeg_std_dc_vals[12];
extern const uint8_t uimg_jpeg_std_ac_lum_bits[17], uimg_jpeg_std_ac_lum_vals[162];
extern const uint8_t uimg_jpeg_std_ac_chr_bits[17], uimg_jpeg_std_ac_chr_vals[162];

// uimg.c owns the one-string-per-process error sink (uimg.h).
void uimg_set_error(const char *msg);

int uimg_jpeg_encode(const struct uimg *im, uint8_t **out, size_t *out_len);

#endif // UIMG_JPEG_INT_H
