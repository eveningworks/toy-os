#ifndef UUI_STACKBAR_H
#define UUI_STACKBAR_H

#include <stdint.h>
#include "ui/ugfx.h"

// A STACKED BAR WITH A LEGEND: how one whole splits into parts, each a
// coloured segment and a legend row (swatch, label, value). Windows 11's
// "Memory composition" and a disk-usage bar are the shape. Not
// uui_progress (one value against a range) or uui_chart (a value over
// time).
//
// The caller formats each value: the widget does not know the unit.
// Takes no input (no `hit`), like uui_meter.

#define UUI_STACKBAR_MAX 8

struct uui_stackbar_seg {
    const char *label;     // the caller's, not copied
    char        value[24]; // shown at the legend row's right
    uint64_t    amount;    // its share of the bar, in any unit
    uint32_t    color;
};

struct uui_stackbar {
    int x, y, w, h;
    struct uui_stackbar_seg seg[UUI_STACKBAR_MAX];
    int count;
    // The bar's whole: 0 means the segments' sum, so the bar is always
    // full; larger leaves the remainder as empty track.
    uint64_t total;
    int legend_cols;       // 0: no legend
    // UUI_COLOR_UNSET (the default) resolves against the theme at DRAW
    // time, so a theme change follows (docs/conventions/gui.md).
    uint32_t track, border, label_fg, value_fg, bg;
};

void uui_stackbar_init(struct uui_stackbar *b, int legend_cols);
// Appends a segment; its index, or -1 when full.
int  uui_stackbar_add(struct uui_stackbar *b, const char *label, uint32_t color);
void uui_stackbar_set(struct uui_stackbar *b, int i, uint64_t amount, const char *value);

void uui_stackbar_natural_size(const struct uui_stackbar *b, int *out_w, int *out_h);
void uui_stackbar_draw(struct ugfx_surface *s, const struct uui_stackbar *b);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_stackbar_ops;

#endif
