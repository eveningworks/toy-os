#ifndef UUI_BUTTON_H
#define UUI_BUTTON_H

#include <stdint.h>
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- buttons and groups ----------------------------------------------

struct uui_button {
    int x, y, w, h;      // content-relative
    const char *label;   // not owned -- must outlive the button
    uint32_t bg, fg;
    int code;            // app-defined id, handed back on a completed click
    int pressed;         // OWNED -- driven by uui_button_group_press/_release
    int hovered;         // OWNED -- driven by uui_button_group_hover
    int disabled;
    int focused;         // OWNED -- the focus ring's set_focused
};

void uui_button_init(struct uui_button *b, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg, int code);

// Repositions without touching pressed/hovered. Separate from init()
// for a real reason inherited from the kernel version: geometry is
// font-size dependent and gets recomputed on redraws, and re-running
// init() every frame would wipe in-progress press state before it could
// ever be drawn.
void uui_button_set_geometry(struct uui_button *b, int x, int y, int w, int h);

// Preferred minimum: the label plus font-derived padding. See
// uui_primitives.h.
void uui_button_natural_size(const struct uui_button *b, int *out_w, int *out_h);

// Draws ONE button in its own current state. The group used to derive
// that state inline, which meant a button could only be painted BY a
// group -- so a button placed by a layout was placed, hit-tested and
// never drawn. See uui_button_ops below.
void uui_button_draw_one(struct ugfx_surface *s, const struct uui_button *b);

// So a button can sit in a uui_layout. See ui/uui_widget.h.
extern const struct uui_widget_ops uui_button_ops;

#endif
