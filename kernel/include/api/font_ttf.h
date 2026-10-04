#ifndef FONT_TTF_H
#define FONT_TTF_H

// Anti-aliased bitmap fonts baked from a real TrueType face
// (JetBrains Mono, OFL 1.1 -- see tools/OFL.txt) at build time by
// tools/genttf.py. Each glyph is a flat grayscale alpha map (0 =
// background, 255 = fully the ink color), rendered once offline
// with real font hinting + anti-aliasing, so it looks like an
// actual font instead of blocky upscaled pixel art -- gfx.c's
// gfx_draw_char() alpha-blends it straight into the framebuffer.
//
// **THIS IS THE FALLBACK NOW, NOT THE ONLY FONT.** There IS a runtime
// rasterizer (userland/lib/ttf.c) and a face loaded from
// /usr/share/fonts (api/font_face.h) takes precedence when one is
// selected. These tables remain because they are the only glyphs that
// need no filesystem, no allocator and no parsing: they draw before
// the disk is mounted, on the panic path, and whenever a font file is
// missing or malformed.
//
// 8 sizes are baked in. gfx_set_font_px() (gfx.c) SNAPS to the nearest
// of them when no face is loaded, which is why an arbitrary size is
// answerable only with one; gfx_font_size() reports which it snapped
// to. The 191-glyph set here is also the set a runtime atlas
// rasterizes, so the two are interchangeable everywhere.
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

#define FONT_TTF_GLYPH_COUNT 191
#define FONT_TTF_ASCII_COUNT 95 // ASCII 32-126, indices 0..94
#define FONT_TTF_EXTRA_FIRST 0xA0 // the Latin-1 Supplement...
#define FONT_TTF_EXTRA_COUNT 96 // ...0xA0-0xFF, indices 95..190

struct font_ttf_variant {
    const unsigned char *glyphs; // FONT_TTF_GLYPH_COUNT * h * w bytes,
                                  // row-major within each w*h glyph
    int w;
    int h;
    const char *name;
};

extern const struct font_ttf_variant font_ttf_variants[FONT_SIZE_COUNT];

// A codepoint's glyph slot, or -1 when the font has none: ASCII 32-126,
// then Latin-1 0xA0-0xFF. ONE definition for both rings -- a ring-3
// atlas must use the same slots or every client reads the wrong glyph.
// Take an UNSIGNED value: a `char` above 0x7F is negative here.
static inline int font_ttf_slot(unsigned int c) {
    if (c >= 32 && c <= 126) return (int)c - 32;
    if (c >= FONT_TTF_EXTRA_FIRST && c < FONT_TTF_EXTRA_FIRST + FONT_TTF_EXTRA_COUNT)
        return FONT_TTF_ASCII_COUNT + (int)(c - FONT_TTF_EXTRA_FIRST);
    return -1;
}

// The inverse: the codepoint a slot holds, or 0 past the end.
static inline unsigned int font_ttf_slot_codepoint(int slot) {
    if (slot < 0 || slot >= FONT_TTF_GLYPH_COUNT) return 0;
    if (slot < FONT_TTF_ASCII_COUNT) return (unsigned int)(32 + slot);
    return FONT_TTF_EXTRA_FIRST + (unsigned int)(slot - FONT_TTF_ASCII_COUNT);
}

// The codepoint a slot is DRAWN with, for a rasteriser: a soft hyphen
// shows as a hyphen, as on xterm and the Linux console.
static inline unsigned int font_ttf_slot_draw_codepoint(int slot) {
    unsigned int cp = font_ttf_slot_codepoint(slot);
    if (cp == 0xAD) return 0x2D;
    return cp;
}

#endif
