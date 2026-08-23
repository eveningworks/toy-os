#ifndef UUI_IMAGE_H
#define UUI_IMAGE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"
#include "lib/uimg.h"

// A decoded picture, in a layout.
//
// The widget owns NO image and decodes nothing: an app hands it a
// `struct uimg` it decoded itself (uimg_load()) and keeps owning it.
// That split is deliberate -- an app usually wants the pixels for other
// reasons too (Image Viewer reports the file's size, the desktop scales
// its wallpaper to the screen rather than to a widget), and a widget
// that owned the decode would make "show the picture I already have"
// the awkward case.
//
// WHAT IT DOES OWN IS THE SCALED COPY, and that is the reason this is a
// widget rather than three lines of ugfx_blit() in each app.
// Resampling a 1280x720 photograph costs tens of milliseconds, a redraw
// happens on every focus change and every damage event, and an app that
// scaled in its draw call would be slow in a way that looks like the
// compositor's fault. So the scale is cached and recomputed only when
// the geometry, the fit mode or the image actually changes.
//
// **A `uui_image` must be released.** uui_image_release() frees that
// cache; a widget that outlives its app's exit is not a problem here,
// but one that is re-pointed at image after image without releasing
// leaks a screen's worth of pixels each time. There is no destructor
// mechanism in this toolkit -- the other widgets own nothing, which is
// why this is the first header that has to say so.

struct uui_image {
    int x, y, w, h;

    // Borrowed, never freed here. NULL draws just the background, which
    // is what an app shows before its first file is loaded.
    const struct uimg *img;

    enum uimg_fit fit;
    uint32_t bg;          // behind and around the picture (letterboxing)

    // What natural_size() will ask for at most. 0 means "the image's own
    // size", which is right for a thumbnail and wrong for a viewer: a
    // 4000px photograph would otherwise demand a 4000px window, and
    // uui_layout has no mechanism to talk it down (it overflows instead
    // -- see CLAUDE.md). An app showing arbitrary files sets these.
    int max_w, max_h;

    // The cache, and the four things it is keyed on. Private.
    struct uimg scaled;
    const struct uimg *cache_src;
    int cache_w, cache_h;
    int cache_fit;
    int cache_failed;     // a scale that ran out of memory; do not retry every frame
};

void uui_image_init(struct uui_image *im, const struct uimg *src, enum uimg_fit fit);

// Point it at a different picture (or NULL). Always invalidates the
// cache, including when the pointer is unchanged -- an app that decodes
// into the same struct has new pixels behind the same address, and
// comparing pointers would show the previous image forever.
void uui_image_set(struct uui_image *im, const struct uimg *src);
void uui_image_set_fit(struct uui_image *im, enum uimg_fit fit);

// Frees the scaled copy. The source image is the caller's.
void uui_image_release(struct uui_image *im);

// The rect the picture is actually drawn in, content-relative -- what a
// test asserts on, and what an app needs to map a click back to a pixel
// of the original. Returns 0 when there is nothing to draw.
int uui_image_drawn_rect(const struct uui_image *im, int *x, int *y, int *w, int *h);

void uui_image_draw(struct ugfx_surface *s, const struct uui_image *im);
void uui_image_natural_size(const struct uui_image *im, int *out_w, int *out_h);

extern const struct uui_widget_ops uui_image_ops;

#endif
