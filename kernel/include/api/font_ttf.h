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
// 8 sizes are baked in; gfx_set_font_size() (gfx.c) picks which
// one gfx_draw_char()/gfx_char_w()/gfx_char_h() actually use.
#include <stddef.h>

enum font_size {
    FONT_SIZE_8,
    FONT_SIZE_10,
    FONT_SIZE_12,
    FONT_SIZE_14,
    FONT_SIZE_16,
    FONT_SIZE_18,
    FONT_SIZE_20,
    FONT_SIZE_24,
    FONT_SIZE_COUNT
};

#define FONT_TTF_GLYPH_COUNT 101
#define FONT_TTF_ASCII_COUNT 95 // ASCII 32-126, indices 0..94
#define FONT_TTF_EXTRA_COUNT 6 // Nordic letters, indices 95..100

struct font_ttf_variant {
    const unsigned char *glyphs; // FONT_TTF_GLYPH_COUNT * h * w bytes,
                                  // row-major within each w*h glyph
    int w;
    int h;
    const char *name;
};

extern const struct font_ttf_variant font_ttf_variants[FONT_SIZE_COUNT];

// Latin-1 codepoints of the FONT_TTF_EXTRA_COUNT glyphs baked after
// the contiguous ASCII block, in baked order -- e.g.
// font_ttf_extra_codepoints[0] == 0xC4 ('\xc4', Ä). See
// font_ttf_glyph_index() (gfx.c) for the codepoint -> glyph-index
// lookup that uses this.
extern const unsigned char font_ttf_extra_codepoints[FONT_TTF_EXTRA_COUNT];

#endif
