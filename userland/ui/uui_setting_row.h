#ifndef UUI_SETTING_ROW_H
#define UUI_SETTING_ROW_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

// ONE SETTING AS A CARD: its name and a line or two of description on
// the left, its control on the right -- or, for a control too wide to sit
// beside the text (a radio list, a long field), under it. The row shape
// of Windows 11 Settings, GNOME's libadwaita ActionRow and Plasma 6's
// FormCard.
//
// A CONTAINER whose one child is the control: the router recurses into
// it, so the control reports its own id to the app and takes focus and
// keys exactly as it would anywhere else. The card, title and description
// are painted in children_begin, under the control.
//
// **THE DESCRIPTION'S ROW COUNT IS A HEIGHT-FOR-WIDTH ANSWER**, and a
// natural_size may not measure from where the widget is. So the count is
// DECIDED OUTSIDE layout: uui_setting_row_fit() computes it from the
// width the last layout gave, and returns 1 when it changed so the caller
// runs the layout again -- which settles, since width never depends on
// height.

struct uui_setting_row {
    int x, y, w, h;

    const char *title;          // caller-owned; bold
    const char *desc;           // caller-owned; may be NULL or ""
    struct uui_item control;    // the one child: ops, widget, id, flags

    // Control under the text rather than beside it. Set it for a control
    // that is tall or wide by nature; fit() also sets `stacked_auto` when
    // the control would leave the text too little room.
    int stacked;
    int stacked_auto;           // OWNED
    int desc_rows;              // OWNED -- fit()'s answer, 1 until then
    int changed;                // mark the card: edited, not yet applied
    int disabled;               // dims the text; the control has its own
    int flat;                   // no card of its own: a row inside a uui_card
};

void uui_setting_row_init(struct uui_setting_row *r, const char *title,
                          const char *desc, struct uui_item control);
// Re-derives `desc_rows` and `stacked_auto` from the current width.
// Returns 1 if either changed, meaning: lay out again.
int  uui_setting_row_fit(struct uui_setting_row *r);
// The width the title and description get -- what a wrap is measured at.
int  uui_setting_row_text_w(const struct uui_setting_row *r);

extern const struct uui_widget_ops uui_setting_row_ops;

#endif
