#ifndef UUI_FONTSAMPLE_H
#define UUI_FONTSAMPLE_H
// A font face loaded to SHOW it, and its sample drawn in a box -- what a
// font picker's card holds: Settings' Appearance > Fonts gallery, and the
// Character Map's face list. Faces are read from their own .ttf through
// uglyph (ring-3 rasterizing, any size), so a face can be shown without
// being the session font; the baked face, which has no file, draws
// through ugfx_font_baked().
//
// A FAMILY is one stem -- dejavu-sans-mono.ttf -- with its bold weight
// beside it as <stem>-bold.ttf, which is a weight and never a family.
#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uglyph.h"

#define UUI_FONT_DIR "/usr/share/fonts"

struct uui_fontface {
    char stem[48];              // the file name without .ttf: what settings store
    char family[48];            // what the file calls itself ("DejaVu Sans Mono")
    struct uglyph_face reg, bold;
    int has_bold;
    int mono;                   // every advance equal (i as wide as W)
};

// Opens <dir>/<stem>.ttf, and its -bold weight when there is one. 1 on
// success; the caller closes it.
int  uui_fontface_open(struct uui_fontface *f, const char *dir, const char *stem);
void uui_fontface_close(struct uui_fontface *f);

// Does this directory entry name a FAMILY (a .ttf that is not a -bold
// weight)? Writes its stem.
int  uui_fontface_family_file(const char *name, char *stem, int cap);

enum uui_fontsample_style {
    // "Aa" large and a pangram beside it, both in the face, on `bg`: a
    // card's picture (the card's label names the face).
    UUI_FONTSAMPLE_CARD,
    // A few lines of a terminal session in the face, light on dark: how
    // a monospace face reads where it is used.
    UUI_FONTSAMPLE_TERMINAL,
};

// Draws `f`'s sample in the box; NULL draws the BAKED face. `dim` mutes
// it, for a face that is a poor fit for the role (a proportional face
// offered as monospace).
void uui_fontsample_draw(struct ugfx_surface *s, struct uui_fontface *f, int style,
                         int x, int y, int w, int h, uint32_t bg, int dim);

#endif
