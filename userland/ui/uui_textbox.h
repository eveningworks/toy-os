#ifndef UUI_TEXTBOX_H
#define UUI_TEXTBOX_H

// Renamed from `uui_field` so the two sides of one widget share a name
// as well as a path: the kernel's version is apps/ui/ui_textbox.h.
// Matching paths were the point of splitting the toolkit one file per
// widget, and mismatched names undercut that -- porting between the two
// should be a file-to-file comparison, not a translation.

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- single-line text field -------------------------------------------

#define UUI_TEXTBOX_MAX 48

struct uui_textbox {
    // Content-relative geometry -- see apps/ui/ui_radio_list.h's note.
    // Unlike the radio list, a field CAN be stretched, so w/h are
    // whatever set_geometry is told (its natural width is 0, meaning
    // "no preference" -- see uui_primitives.h).
    int x, y, w, h;

    char buf[UUI_TEXTBOX_MAX]; // NUL-terminated
    int len;
    int cursor; // [0, len]
    int active; // 1 = focused: draws a caret and accepts keys
};

void uui_textbox_init(struct uui_textbox *f, const char *initial);
void uui_textbox_set_active(struct uui_textbox *f, int active);

// Returns 1 if the key was consumed. Deliberately does NOT consume
// Enter: "commit this field" is the caller's decision, not the widget's.
int uui_textbox_key(struct uui_textbox *f, int key);

// Height only: one row plus insets. **Width is 0 -- no preference**
// (see uui_primitives.h): a field's width is whatever the form gives it.
void uui_textbox_natural_size(const struct uui_textbox *f, int *out_w, int *out_h);

// Draws the field, scrolling its content horizontally so the caret
// stays visible -- typing past the right edge behaves like a real text
// input rather than drawing through the border.
void uui_textbox_set_geometry(struct uui_textbox *f, int x, int y, int w, int h);

// Point (cx, cy) inside the field, content-relative.
int uui_textbox_hit(const struct uui_textbox *f, int cx, int cy);

void uui_textbox_draw(struct ugfx_surface *s, const struct uui_textbox *f,
                     uint32_t bg, uint32_t fg, uint32_t border);

// The focus ring's ops table for a field: hit/key/set_focused only, the
// drawing and layout slots left NULL (every slot is optional -- see
// uui_widget.h). A field's draw takes three colours the generic
// signature cannot carry, so a full table here would be half-honest.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_textbox_focus_ops;

#endif
