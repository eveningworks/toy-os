#ifndef UUI_CHART_H
#define UUI_CHART_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

// chart -- a percentage over TIME, which is the one thing uui_meter
// cannot show.
//
// A meter answers "how busy is it now"; this answers "and was it busy a
// minute ago", which is a different question and the reason Task
// Manager, GNOME System Monitor and KSysGuard all carry both. The
// sample is deliberately a PERCENTAGE rather than a free number: every
// caller so far is a proportion of something, a fixed 0..100 range needs
// no axis negotiation, and a chart that rescales under a moving series
// is one whose shape means nothing between two glances.
//
// **THE RING IS INSIDE THE WIDGET AND IT IS SMALL.** One byte per
// sample, so the whole history costs less than the struct around it --
// Toykit has no allocator, and a caller-supplied array would be one more
// thing for every caller to size wrongly.
//
// NEWEST ON THE RIGHT, which is every system monitor's direction and
// the one that keeps the eye where new data arrives.

#define UUI_CHART_MAX 180   // at Task Manager's cadence, about three minutes

struct uui_chart {
    int x, y, w, h;
    const char *label;          // drawn top-left, or NULL for none
    uint8_t samples[UUI_CHART_MAX];
    int count;                  // valid samples, capped at UUI_CHART_MAX
    int head;                   // where the next sample goes
    // UUI_COLOR_UNSET lets the theme answer at DRAW time -- see utheme.h
    // on why a widget must not resolve its colours when it is built.
    uint32_t bg, grid, line, fill;
};

void uui_chart_init(struct uui_chart *c, const char *label);

// Append one reading, 0..100. Out-of-range values are CLAMPED rather
// than refused: a caller computing a percentage from two deltas can
// legitimately land just outside on a rounding edge, and a dropped
// sample would leave a hole in a series that is read as a shape.
void uui_chart_push(struct uui_chart *c, int percent);

// The most recent sample, or 0 when there is none. For a caller that
// wants to print the number beside the trace.
int uui_chart_last(const struct uui_chart *c);

void uui_chart_natural_size(const struct uui_chart *c, int *out_w, int *out_h);
void uui_chart_set_geometry(struct uui_chart *c, int x, int y, int w, int h);
void uui_chart_draw(struct ugfx_surface *s, const struct uui_chart *c);

extern const struct uui_widget_ops uui_chart_ops;

#endif
