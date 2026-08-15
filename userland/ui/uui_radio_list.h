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
    // Content-relative geometry, matching every other widget here. It
    // used to have none: draw() and hit() were each told where the
    // control was, separately, so nothing else could ask. See
    // apps/ui/ui_radio_list.h for the full note.
    int x, y, w, h;

    const char *const *options; // caller-owned
    int count;
    int cols;        // 1 = a plain vertical list
    int row_h;       // full row height including its gap
    int col_w;
    int marker_size;
};

// Preferred minimum: the grid its columns and rows need. See
// uui_primitives.h. Renamed from uui_radio_list_size().
void uui_radio_list_natural_size(const struct uui_radio_list *l, int *out_w, int *out_h);
// Positions the control; w/h follow from the grid via
// uui_radio_list_natural_size(), since a radio list cannot be stretched
// into a size its rows and columns don't produce.
void uui_radio_list_set_geometry(struct uui_radio_list *l, int x, int y);

void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l,
                          int selected, int hovered,
                          uint32_t bg, uint32_t fg);
// Index under the content-relative point (cx, cy), or -1.
int uui_radio_list_hit(const struct uui_radio_list *l, int cx, int cy);

#endif
