#ifndef ULIB_UCURSOR_H
#define ULIB_UCURSOR_H

#include <stdint.h>

// One cursor SHAPE, as /usr/share/cursors/<theme>/<shape> describes it --
// the file format, shared by the compositor (userland/wm/cursor_theme.c,
// which owns the live theme) and anything that only SHOWS a theme
// (System Settings' gallery). Two kinds, Xcursor's split:
//   - MASKS: an outline and a fill coverage grid, coloured at draw time
//     (a black rim under `fill_rgb`). `default` and `bold` are masks.
//   - an IMAGE: straight-alpha ARGB in a QOI file beside the descriptor,
//     whose colours are its own, up to UCURSOR_SCALE_MAX pre-rendered
//     scales so a large pointer is redrawn art rather than doubled pixels.

#define UCURSOR_DIR "/usr/share/cursors"
// The authored size cap AT 1x, for both kinds, and the masks' storage.
#define CURSOR_SHAPE_MAX 32
// The largest size setting, and so the largest pre-rendered scale.
#define CURSOR_SCALE_MAX 3
// An image file's name as the descriptor gives it: "arrow@2x.qoi".
#define CURSOR_IMAGE_NAME_MAX 32
// A whole descriptor; bigger is refused, not truncated.
#define CURSOR_FILE_MAX 4096

struct cursor_shape {
    int loaded;              // 0 means "use the built-in fallback"
    int w, h;                // AT 1x, whichever kind
    int hot_x, hot_y;        // the pixel that IS the pointer position, at 1x
    unsigned char outline[CURSOR_SHAPE_MAX][CURSOR_SHAPE_MAX];
    unsigned char fill[CURSOR_SHAPE_MAX][CURSOR_SHAPE_MAX];

    // An IMAGE shape: `image[k-1]` names the k-x file ("" if absent);
    // ucursor_decode() fills `px` -- w*baked by h*baked straight-alpha
    // 0xAARRGGBB, malloc'd, freed by ucursor_release().
    int is_image;
    char image[CURSOR_SCALE_MAX][CURSOR_IMAGE_NAME_MAX];
    int baked;               // the scale `px` was rendered at, 1..3
    uint32_t *px;
};

// Parses one DESCRIPTOR; pure, no filesystem (an image shape only NAMES
// its files). 1 on success, 0 on any malformation -- it REJECTS rather
// than guesses. TRAP: it overwrites `out` wholesale, `px` included, so
// release an image shape before handing it back here, or it leaks.
int cursor_shape_parse(const char *text, uint32_t len, struct cursor_shape *out);

// Decodes an image shape's rendering for `scale` (the 1x one when the
// theme has none for it). A file whose size is not exactly scale times
// the descriptor's is refused. 1 on success; on failure `why` (if any)
// holds a sentence for the caller's log and the shape is untouched.
int ucursor_decode(const char *theme, struct cursor_shape *s, int scale,
                   char *why, int why_len);

// Frees an image shape's pixels and marks it unloaded. Safe on any shape.
void ucursor_release(struct cursor_shape *s);

// Reads, parses and (for an image) decodes UCURSOR_DIR/<theme>/<name> at
// `scale`, for a caller with no file layer of its own. 1 on success; an
// absent shape is 0 and not an error -- a theme need not be complete.
// Not reentrant: one static read buffer.
int ucursor_load(const char *theme, const char *name, int scale, struct cursor_shape *out);

// The pixel at offset (dx, dy) from the top-left, where each stored pixel
// covers `step` drawn pixels: an image's own ARGB, or a mask pair folded
// into straight ARGB as `fill_rgb` over a black rim. 0 outside.
uint32_t ucursor_pixel(const struct cursor_shape *s, int dx, int dy, int step,
                       uint32_t fill_rgb);

#endif // ULIB_UCURSOR_H
