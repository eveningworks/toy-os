#ifndef UUI_UAMBIENT_H
#define UUI_UAMBIENT_H

// A STAGE TINTED BY A PICTURE (YouTube's "ambient mode"): the colours a
// picture's surroundings take from it, and the radial ground painted in
// them. The Image Viewer's stage and the Audio Player's, behind a photo
// and behind a cover; docs/gui-guidelines.md's "content is the hero".
#include <stdint.h>
#include "ui/ugfx.h"
#include "lib/uimg.h"

struct uambient {
    uint32_t centre, edge;   // the stage: lit middle, deep edge
    uint32_t strip;          // a band under it (the viewer's filmstrip)
    uint32_t chrome;         // UTHEME_CHROME washed FAINTLY with the picture
    uint32_t chrome_line;    // the hairline a step darker
    uint32_t panel;          // a side pane a step lighter
};

// The neutral stage, for no picture.
void uambient_default(struct uambient *a);
// From a picture: the stage runs from its darkened average at the centre
// to a deep shade of its darkest quarter at the edge -- a sunset sits in
// warm dusk, a seascape in deep teal. Falls back to the default.
void uambient_from(struct uambient *a, const struct uimg *im);

// The radial ground, CACHED per size and colour pair: computed once,
// blitted every frame after. Zero-initialise; uambient_stage_free().
struct uambient_stage { uint32_t *grad; int w, h; uint32_t c, e; };
void uambient_paint(struct uambient_stage *st, const struct uambient *a,
                    struct ugfx_surface *s, int x, int y, int w, int h);
void uambient_stage_free(struct uambient_stage *st);

#endif
