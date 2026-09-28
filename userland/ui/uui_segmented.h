#ifndef UUI_SEGMENTED_H
#define UUI_SEGMENTED_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"

// A SEGMENTED CONTROL: two to four mutually-exclusive choices as one row
// of joined buttons, the chosen one filled -- macOS's NSSegmentedControl,
// Windows' and KDE's view switchers. The same data as uui_radio_list
// (`options`, an index out), for when the choices are few and SHORT
// enough to sit side by side.
//
// Commits on RELEASE over the segment that was pressed, like a button;
// the arrows move the choice while it has focus. Segments share one width
// -- the widest label's -- so the row does not look like a ragged table.

#define UUI_SEGMENTED_MAX 8

struct uui_segmented {
    int x, y, w, h;                 // w/h are the natural size
    const char *const *options;     // caller-owned
    int count;
    int selected;                   // the value, or -1
    int hovered, armed;             // OWNED: segment indices, or -1
    int focused;                    // OWNED
    int disabled;
    uint32_t bg, fg;                // UUI_COLOR_UNSET = the theme's
};

void uui_segmented_init(struct uui_segmented *sg, const char *const *options,
                        int count, int selected);
void uui_segmented_natural_size(const struct uui_segmented *sg, int *out_w, int *out_h);
void uui_segmented_set_geometry(struct uui_segmented *sg, int x, int y);
void uui_segmented_draw(struct ugfx_surface *s, const struct uui_segmented *sg);
// The segment under (cx, cy), or -1.
int  uui_segmented_at(const struct uui_segmented *sg, int cx, int cy);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_segmented_ops;

#endif
