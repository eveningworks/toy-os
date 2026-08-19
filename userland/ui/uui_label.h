#ifndef UUI_LABEL_H
#define UUI_LABEL_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"

// A line of text the LAYOUT knows about.
//
// The missing primitive. Every app here that wanted a caption drew it in
// on_draw and worked out its own coordinates -- which is fine until the
// thing it captions moves, and until the page scrolls. System Settings
// hit both in one day: a page heading painted in on_draw landed on top
// of the first control (on_draw runs AFTER the toolkit paints widgets),
// and reserving space for it by hand would have left content sliding
// underneath it as the page scrolled.
//
// So: a widget with no behaviour and no input, whose entire job is to
// occupy a row the layout has accounted for. It takes no press, no key
// and no focus -- `hit` is absent, so the router never offers it
// anything and a click passes through to whatever is behind.
//
// The text is POINTED AT, not copied: a caption is usually a literal or
// an app-owned buffer, and copying would mean a size limit and a second
// place the string lives. A buffer must outlive the label.

struct uui_label {
    int x, y, w, h;
    const char *text;   // caller-owned; NULL draws nothing
    uint32_t fg, bg;
    // Reserve room for this many rows even when `text` is shorter or
    // NULL, so a caption that changes cannot reflow the page under the
    // user. 0 means one row.
    int rows;
};

void uui_label_init(struct uui_label *l, const char *text);
void uui_label_set_text(struct uui_label *l, const char *text);
void uui_label_natural_size(const struct uui_label *l, int *out_w, int *out_h);
// CLIPPED to its own width, always: ugfx_draw_string does not clip, and
// a caption longer than its column would otherwise run into whatever is
// beside it -- the identical overlap bug this codebase has shipped
// twice (docs/gui-guidelines.md).
void uui_label_draw(struct ugfx_surface *s, const struct uui_label *l);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_label_ops;

#endif
