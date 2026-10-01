#ifndef UIMG_H
#define UIMG_H

#include <stdint.h>
#include <stddef.h>

// uimg -- decoding an image file into pixels, in RING 3.
//
// WHY THIS IS NOT IN THE KERNEL, stated once because it is the whole
// shape of the thing. No mainstream kernel decodes images: Linux's only
// in-kernel image is the boot logo, an UNCOMPRESSED ppm turned into a C
// array at build time, and JPEG/PNG live in userspace (libjpeg-turbo,
// libpng). Windows keeps codecs in WIC, a user-mode pluggable codec
// framework. Under Wayland a client hands the compositor a buffer of
// pixels and the compositor never parses a file format at all -- KWin
// and Mutter decode a wallpaper in the shell process, through Qt or
// GdkPixbuf, and pass the result down.
//
// toy-os follows rather than differs, and the contrast with ttf.h is
// the argument: the kernel needs GLYPHS before any process exists (the
// console draws text at boot), so its font parser had to be in ring 0
// and every read in it is bounds-checked for that reason. Nothing in
// ring 0 needs an IMAGE. So this lives in libuapp.a, a malformed file
// can at worst take down the one process that opened it, and the kernel
// gained no attack surface at all.
//
// THE CODEC TABLE IS THE EXTENSION POINT. A format is a `struct
// uimg_codec` row in uimg.c -- probe by magic bytes, then info/decode
// -- which is the same registry shape display_driver, block_device and
// clocksource already use here, and what WIC/GdkPixbuf/Qt's image
// plugins are. Adding PNG or QOI is a file and a row, not a second
// mechanism and not an `if (jpeg) ... else if (png)` chain growing a
// branch per format.
//
// ALLOCATION: this library DOES allocate, unlike ttf.h. It is ring-3
// only, malloc is real here (kernel/lib/heap_core.c compiled twice), and
// an image's size is not known until its header has been parsed -- a
// caller-supplied buffer would mean every caller carrying a worst-case
// one. Every allocation is paired with uimg_free().

// A decoded image. `px` is w*h pixels of 0xAARRGGBB -- the low three
// bytes are exactly what ugfx_blit() takes and what ugfx_rgb() packs, so
// an OPAQUE image is a blit with no conversion pass.
//
// **THE TOP BYTE IS ALPHA AND IT IS ONLY MEANINGFUL HERE**, not in a
// ugfx surface: a surface is 0x00RRGGBB, because the framebuffer has no
// alpha to composite against. A codec that has no alpha (JPEG) fills
// 0xFF, so `px` is uniform whatever produced it and a caller never has
// to ask which codec it came from -- it asks `has_alpha`.
struct uimg {
    int w, h;
    uint32_t *px;    // malloc'd by the decode/scale calls; uimg_free() releases it

    // Does any pixel have alpha < 255? Set by the codec, preserved by
    // uimg_scale(). It exists so a CALLER can pick the cheap path:
    // compositing a 1280x720 wallpaper per-pixel when every pixel is
    // opaque is nearly a million pointless blends per repaint, and
    // ugfx_blit() is a straight copy.
    int has_alpha;
};

// What a file SAYS it is, without decoding it. Cheap: parsing a JPEG's
// headers is a few hundred bytes of work whatever the image's size.
struct uimg_info {
    int w, h;
    int components;         // 1 = grayscale, 3 = colour
    const char *format;     // the codec's name, e.g. "jpeg"
    char detail[64];        // codec-specific, e.g. "baseline, 4:2:0, restart markers"
};

struct uimg_codec {
    const char *name;

    // Does this look like my format? Magic bytes only -- a probe must
    // not be expensive and must not be clever, because it runs on every
    // codec in turn. Returns 1 for mine.
    int (*probe)(const uint8_t *d, size_t n);

    // Both return 0 or a negative errno. See the error codes below.
    int (*info)(const uint8_t *d, size_t n, struct uimg_info *out);

    // NULL means this build can read the format's header but not its
    // pixels -- uimg_decode() turns that into -ENOTSUP with a sentence
    // saying so, which is the honest answer and not the same as
    // "corrupt file". No row is NULL today; PNG was, until it gained a
    // decoder, and the mechanism stays because the next format to arrive
    // write-first should not have to invent it again.
    int (*decode)(const uint8_t *d, size_t n, struct uimg *out);

    // Writes `im` into a fresh allocation, `*out_len` bytes, which the
    // caller frees. NULL for a format this build only reads.
    int (*encode)(const struct uimg *im, uint8_t **out, size_t *out_len);
};

// ERRORS ARE NEGATIVE ERRNOS, the same convention a failed syscall
// follows here (see docs/conventions/kernel.md), and the three that
// matter are DISTINCT ON PURPOSE:
//
//   -EINVAL   the bytes are malformed, truncated, or not an image at
//             all. The file is broken.
//   -ENOTSUP  a valid file this build refuses: arithmetic-coded or
//             lossless JPEG, 12-bit samples, CMYK. The file is fine; we
//             are not. That distinction is what lets an app say "this
//             build cannot read an arithmetic-coded JPEG" instead of
//             "corrupt file", which would be a lie.
//   -ENOMEM   the pixels did not fit.
//
// uimg_last_error() carries the human sentence -- libjpeg's
// jpeg_error_mgr message, in effect. It is a static string set by the
// last failing call in this process; there are no threads here.
const char *uimg_last_error(void);

// Which codec claims these bytes, or NULL. `n` may be short: 8 bytes is
// enough for every probe here.
const struct uimg_codec *uimg_probe(const void *data, size_t n);

// Header only / full decode, from a buffer the caller owns.
int uimg_info(const void *data, size_t n, struct uimg_info *out);
int uimg_decode(const void *data, size_t n, struct uimg *out);

// The same two, from a path. The file is read into a temporary
// allocation and released before returning, so a decoded image costs
// its pixels and not also its file.
//
// UIMG_MAX_FILE is a REFUSAL, not a truncation: half a JPEG decodes to
// half a picture and a plausible-looking one, which is worse than an
// error (the same rule kfmt's formatters and fs_read_into() follow).
#define UIMG_MAX_FILE (8u * 1024u * 1024u)
int uimg_load_info(const char *path, struct uimg_info *out);
int uimg_load(const char *path, struct uimg *out);

// Releases `im->px` and zeroes the struct. Safe on an already-freed or
// never-decoded image.
void uimg_free(struct uimg *im);

// --- encoding ---------------------------------------------------------
//
// The other direction, and the only reason it exists is that something
// had to WRITE a file: the screenshot tool. Three formats, and the set
// is the point, as it is for the decoders above:
//
//   jpeg what a PHOTOGRAPH should be written as -- lossy, opaque, and
//        roughly a tenth of the PNG for the same picture. Never for UI
//        content, where the DCT puts ringing around every glyph.
//
//   qoi  the DEFAULT for a screenshot, because it is a couple of
//        hundred lines, needs no compression library, and on flat UI
//        content it beats PNG anyway (measured: 87 KB against 91 KB for
//        the same 1280x720 desktop).
//   png  what LEAVES the machine, and what arrives from outside it. Both
//        directions work now (lib/uinflate.h); a PNG is what a host, a
//        browser or a bug report expects, and what most images anyone
//        brings to this system already are.
//
// A format is named, never guessed from the pixels. `uimg_save()` picks
// it from the path's extension because that is what a user typing a
// filename means by it.

// Encodes into a fresh allocation. `*out` is the caller's to free().
// `format` is a codec name ("qoi", "png"). Returns 0, -EINVAL for an
// empty image, -ENOTSUP for a format this build cannot write, -ENOMEM.
int uimg_encode(const struct uimg *im, const char *format,
                uint8_t **out, size_t *out_len);

// The same, straight to a path. `format` may be NULL, which takes it
// from the extension; an unknown extension is -ENOTSUP rather than a
// guess, for the reason a parser here never guesses.
int uimg_save(const char *path, const struct uimg *im, const char *format);

// --- scaling ---------------------------------------------------------
//
// How a picture is placed in a box that is not its shape. The names are
// CSS's object-fit, which is also what a wallpaper picker means by
// "Fit"/"Fill"/"Stretch" on every desktop that has one.
enum uimg_fit {
    UIMG_FIT_NONE,     // no scaling; the image is centred and may be cropped
    UIMG_FIT_CONTAIN,  // fits entirely inside the box, aspect kept, letterboxed
    UIMG_FIT_COVER,    // covers the box, aspect kept, overflow cropped
    UIMG_FIT_STRETCH,  // fills the box exactly, aspect broken
};

// The size an image of sw x sh takes in a bw x bh box under `mode`.
// Never returns 0 in either axis (a 1px result is clamped up), because
// a zero-sized scale target is an allocation of nothing and then a
// divide by it.
void uimg_fit_size(int sw, int sh, int bw, int bh, enum uimg_fit mode,
                   int *out_w, int *out_h);

// Resamples `src` to dw x dh into a freshly allocated `out`.
//
// SEPARABLE, TWO PASSES, AND THE FILTER IS CHOSEN PER AXIS: an axis
// being shrunk is BOX-AVERAGED (every source pixel contributes, which
// is what stops a downscaled photo turning into aliased noise -- the
// visible failure of the nearest-neighbour version this replaced), an
// axis being enlarged is LINEARLY interpolated. Nearest-neighbour is
// available nowhere here on purpose; it is only ever the right answer
// for pixel art, which this system has none of.
//
// All integer: there is no floating point in this project, in either
// ring. Weights are 16.16 fixed point, accumulated in 32-bit and
// normalised once per output pixel.
int uimg_scale(const struct uimg *src, int dw, int dh, struct uimg *out);

// `src` turned `quarters` quarter turns CLOCKWISE (any integer; taken mod
// 4) into a freshly allocated `out` -- a 90-degree turn swaps w and h.
// Exact, no resampling: a viewer's "rotate" must not soften the picture.
int uimg_rotate(const struct uimg *src, int quarters, struct uimg *out);

// --- codecs -----------------------------------------------------------
//
// Two rows in uimg.c's table, and the pair is the point: JPEG is what a
// camera produces -- lossy, opaque, big -- and QOI is what an ICON needs
// -- lossless, with an alpha channel, and small enough at 64x64 that the
// file is smaller than the JPEG header would be. Neither is a substitute
// for the other, and a decoder that had to be one would be a worse
// version of both.
// PNG is the third row, and the only one that both reads and writes
// through a compression library (lib/uinflate.h) rather than its own
// code. See the encoding section above.
extern const struct uimg_codec uimg_codec_jpeg;
extern const struct uimg_codec uimg_codec_qoi;
extern const struct uimg_codec uimg_codec_png;

#endif
