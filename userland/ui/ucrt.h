#ifndef UCRT_H
#define UCRT_H

// A CRT SCREEN EFFECT, as CPU passes over one rect of a surface: phosphor
// glow, scanlines, a phosphor mask, a vignette, flicker, static noise,
// and a curved tube in a bezel. The Terminal runs it over its grid; it
// is written for any app that draws a picture of its own (DOOM next).
//
// **AN APP RUNS IT AFTER IT HAS DRAWN, OVER ITS OWN PIXELS** -- cool-
// retro-term's and Windows Terminal's shape (`retroTerminalEffect`), not
// a compositor's. There is no GPU here, so every pass is CPU time on
// every redrawn frame, and only the app knows how rarely it redraws.
//
// **CURVATURE MOVES WHAT IS WHERE.** ucrt_source_point() turns a point on
// the glass back into the point that was drawn there -- the app's pointer
// code must ask it, or a click lands on the wrong cell -- and
// ucrt_margin() is how far in from the rect's edge a picture must keep
// its content for the curve not to cut its corners off.
//
// The context owns its buffers (malloc'd on first use, rebuilt when the
// rect's size or the curve changes); ucrt_free() releases them. A failed
// allocation leaves the rect as it was drawn: plain, never garbage.
#include <stdint.h>
#include "ui/ugfx.h"

#define UCRT_LEVEL_MAX 3       // scanlines/glow/vignette: off, low, medium, high

enum ucrt_curve { UCRT_CURVE_OFF, UCRT_CURVE_SUBTLE, UCRT_CURVE_STRONG, UCRT_CURVE_COUNT };
enum ucrt_mask  { UCRT_MASK_OFF, UCRT_MASK_APERTURE, UCRT_MASK_SLOT, UCRT_MASK_COUNT };

struct ucrt_look {
    int scanlines, glow, vignette;   // 0..UCRT_LEVEL_MAX
    int curve;                       // enum ucrt_curve
    int mask;                        // enum ucrt_mask
    int flicker, noise;              // 0/1 -- both ANIMATE the window (ucrt_animates)
};

// The presets the Options page offers, in its order.
enum { UCRT_PRESET_SUBTLE, UCRT_PRESET_CLASSIC, UCRT_PRESET_CURVED, UCRT_PRESET_COUNT };
extern const struct ucrt_look ucrt_presets[UCRT_PRESET_COUNT];
extern const char *const ucrt_preset_names[UCRT_PRESET_COUNT];
// Which preset `l` is, or -1 for a look of its own.
int ucrt_preset_of(const struct ucrt_look *l);

int ucrt_look_on(const struct ucrt_look *l);       // does anything at all
int ucrt_animates(const struct ucrt_look *l);      // must be redrawn every frame

struct ucrt {
    struct ucrt_look look;
    int period;          // scanline pitch in pixels, >= 2; the app's (a font's cell / 6)
    // 0, or the rows the picture was DRAWN at, scaled up to fill the
    // rect (DOOM's 200): then a scanline closes each of those rows,
    // however many pixels it got, and `period` only sizes the glow.
    int src_lines;
    uint32_t bezel;      // what is not glass, under a curve

    // Built by ucrt_apply() -- not the app's.
    int w, h, map_curve;
    uint32_t *map;       // per pixel: source index << 8 | x weight << 4 | y weight, or UCRT_BEZEL
    uint32_t *buf;       // the rect as composed, for the curve to warp from
    uint32_t *half, *half2;   // the glow, at half resolution
    void *col_sums;           // its column blur's running sums
    uint64_t *lanes;          // per column, as 16-bit lanes: the glow, the gain, the two masks
    int hw, hh;
    unsigned frame;
    uint32_t rng;
};

void ucrt_init(struct ucrt *c);
void ucrt_free(struct ucrt *c);

// Pixels a picture of `w` x `h` must keep clear at its edges so the
// curve keeps every corner on the glass; 0 with no curve.
int ucrt_margin(const struct ucrt_look *l, int w, int h);

// Runs c->look over the rect, in place. 0, or -1 when there was no
// memory for it (the rect is left as drawn).
int ucrt_apply(struct ucrt *c, struct ugfx_surface *s, int x, int y, int w, int h);

// The same, reading the picture from `src` (`stride` pixels a row, the
// rect's top-left first) and only WRITING the surface -- for a
// UAPP_SCANOUT app, whose surface may be write-combining memory that is
// ruinous to read. On failure the picture is copied across plain.
int ucrt_apply_from(struct ucrt *c, const uint32_t *src, int stride,
                    struct ugfx_surface *s, int x, int y, int w, int h);

// The rect-relative point that was DRAWN where (px, py) now shows: 1,
// or 0 on the bezel (with the nearest point on the glass in *sx/*sy).
// Identity with no curve. Valid after the first ucrt_apply().
int ucrt_source_point(const struct ucrt *c, int px, int py, int *sx, int *sy);

#endif
