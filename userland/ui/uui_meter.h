#ifndef UUI_METER_H
#define UUI_METER_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"

// A MEASURED VALUE, shown big: a caption, a number, its unit, an
// optional detail line and an optional fill bar.
//
// The gap it fills. A label is a line of text and a progress bar is a
// fraction; neither is "here is a reading, and here is how it compares
// to the scale it lives on". Every app that has wanted one so far drew
// it in `on_draw` with hand-computed coordinates, which is the same
// thing `uui_label` was added to stop.
//
// **NOTHING IS COPIED.** The three strings are POINTED AT, like
// `uui_label`'s text, so a caller formats into its own buffer and that
// buffer must outlive the meter. Copying would mean a size limit and a
// second place the value lives -- and a meter's whole job is to show a
// number that changes.
//
// It takes no input: no `hit`, so the router never offers it a click and
// one passes through to whatever is behind. That is deliberate -- a
// reading is not a control.

struct uui_meter {
    int x, y, w, h;

    // All caller-owned; NULL draws nothing for that part.
    const char *caption;   // "SEQ1M Q1T1 READ" -- small, above the number
    const char *value;     // "182.4" -- the big one. Formatted by the caller:
                           // a widget cannot know the right precision.
    const char *unit;      // "MB/s" -- beside/below the value
    const char *detail;    // an optional second line: "2954 IOPS, 0.34 ms"

    // ALL FOUR STRING ROWS AND THE BAR ARE RESERVED whether or not you
    // set them, so this widget's height does NOT change when its value
    // does -- see the .c file for the layout overflow that rule exists
    // to prevent. A meter is the same size empty as full.

    // The fill bar, in PER MILLE (0..1000) so a caller with integer
    // arithmetic can express a fraction -- there is no floating point
    // in this project's kernel-shared code and apps here follow suit.
    // NEGATIVE means "no bar", which is not the same as zero: an empty
    // bar says the value is at the bottom of its scale, and no bar says
    // there is no scale. Values above 1000 are CLAMPED rather than
    // drawn overflowing.
    int fill;

    // NULL for all three means the theme's own colours, so a meter
    // declared with nothing but its strings still looks right.
    uint32_t fg, bg, accent;

    // WHICH FONT THE BIG NUMBER IS IN, and it affects MEASUREMENT as
    // well as drawing -- the same rule and the same reason as
    // uui_label's `font` field: natural_size() is asked by the layout
    // long before any painting, so a value measured in one font and
    // drawn in another lands in a box sized for something else. NULL
    // means the session's bold weight.
    const struct ugfx_font *value_font;
};

// Sets the palette from the theme and everything else to empty. Call
// before setting any field; a meter used without it draws with a zero
// palette, i.e. black on black.
void uui_meter_init(struct uui_meter *m);

// The four strings in one call, because they are only ever meaningful
// together -- a caption with a stale value beside it is worse than
// either alone. Pass NULL for `detail` when there is none.
void uui_meter_set(struct uui_meter *m, const char *caption,
                   const char *value, const char *unit, const char *detail);

// The bar, 0..1000 per mille; negative removes it. Separate from
// _set() because a caller often knows the value long before it knows
// what to compare it against.
void uui_meter_set_fill(struct uui_meter *m, int per_mille);

void uui_meter_natural_size(const struct uui_meter *m, int *out_w, int *out_h);
void uui_meter_draw(struct ugfx_surface *s, const struct uui_meter *m);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_meter_ops;

#endif
