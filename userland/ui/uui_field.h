#ifndef UUI_FIELD_H
#define UUI_FIELD_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- single-line text field -------------------------------------------

#define UUI_FIELD_MAX 48

struct uui_field {
    char buf[UUI_FIELD_MAX]; // NUL-terminated
    int len;
    int cursor; // [0, len]
    int active; // 1 = focused: draws a caret and accepts keys
};

void uui_field_init(struct uui_field *f, const char *initial);
void uui_field_set_active(struct uui_field *f, int active);

// Returns 1 if the key was consumed. Deliberately does NOT consume
// Enter: "commit this field" is the caller's decision, not the widget's.
int uui_field_key(struct uui_field *f, int key);

// Draws the field, scrolling its content horizontally so the caret
// stays visible -- typing past the right edge behaves like a real text
// input rather than drawing through the border.
void uui_field_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                     const struct uui_field *f,
                     uint32_t bg, uint32_t fg, uint32_t border);

#endif
