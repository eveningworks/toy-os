#ifndef UUI_RADIO_LIST_H
#define UUI_RADIO_LIST_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- radio list -------------------------------------------------------

struct uui_radio_list {
    const char *const *options; // caller-owned
    int count;
    int cols;        // 1 = a plain vertical list
    int row_h;       // full row height including its gap
    int col_w;
    int marker_size;
};

void uui_radio_list_size(const struct uui_radio_list *l, int *out_w, int *out_h);
void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l,
                          int x, int y, int selected, int hovered,
                          uint32_t bg, uint32_t fg);
// Index under (px, py), or -1.
int uui_radio_list_hit(const struct uui_radio_list *l, int x, int y, int px, int py);

#endif
