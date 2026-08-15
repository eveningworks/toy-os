#ifndef UUI_CHECKBOX_H
#define UUI_CHECKBOX_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- checkbox ---------------------------------------------------------

// Width of the whole clickable area: box + gap + label.
int uui_checkbox_width(int size, const char *label);

void uui_checkbox_draw(struct ugfx_surface *s, int x, int y, int size,
                        int checked, int hovered, const char *label,
                        uint32_t bg, uint32_t fg);

// Hit-tests box AND label -- a highlight larger than its target is a
// lie about where to click, and so is the reverse.
int uui_checkbox_hit(int x, int y, int size, const char *label, int px, int py);

#endif
