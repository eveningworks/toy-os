#ifndef FONT_TTF_H
#define FONT_TTF_H

// Anti-aliased bitmap fonts baked from a real TrueType face
// (JetBrains Mono, OFL 1.1 -- see tools/OFL.txt) at build time by
// tools/genttf.py. Each glyph is a flat grayscale alpha map (0 =
// background, 255 = fully the ink color), rendered once offline
// with real font hinting + anti-aliasing, so it looks like an
// actual font instead of blocky upscaled pixel art -- gfx.c's
// gfx_draw_char() alpha-blends it straight into the framebuffer,
// no runtime rasterization involved.
//
// 4 sizes are baked in; gfx_set_font_size() (gfx.c) picks which
// one gfx_draw_char()/gfx_char_w()/gfx_char_h() actually use.
#include <stddef.h>

enum font_size {
    FONT_SIZE_TINY,
    FONT_SIZE_SMALL,
    FONT_SIZE_MEDIUM,
    FONT_SIZE_LARGE,
    FONT_SIZE_COUNT
};

#define FONT_TTF_GLYPH_COUNT 95

struct font_ttf_variant {
    const unsigned char *glyphs; // FONT_TTF_GLYPH_COUNT * h * w bytes,
                                  // row-major within each w*h glyph
    int w;
    int h;
    const char *name;
};

extern const struct font_ttf_variant font_ttf_variants[FONT_SIZE_COUNT];

#endif
