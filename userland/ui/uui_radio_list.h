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

    // OWNED by the widget, so a routed press has somewhere to record a
    // choice. draw() still takes `selected` as an argument (callers
    // pass `l->selected`) rather than reading this, which is why
    // nothing that already worked had to change.
    int selected;
    int hovered;   // OWNED -- driven through the ops table's motion
    int focused;   // OWNED -- driven by the focus ring's set_focused

    // What `selected` was before the press currently in flight, so a
    // press dragged off the list can put it back. -1 means no press is
    // armed. See the ops table's press/release pair: this widget arms
    // on press and COMMITS ON RELEASE, which is what
    // docs/gui-guidelines.md requires of every control here -- and what
    // it did not do until a Control Panel that acted on every routed
    // event, motion included, wrote a setting to disk per mouse move.
    int armed_prev;

    // Own colours, for the same reason the textbox has them: a generic
    // draw slot cannot carry them as arguments.
    uint32_t bg, fg;
};

// Preferred minimum: the grid its columns and rows need. See
// uui_primitives.h. Renamed from uui_radio_list_size().
void uui_radio_list_natural_size(const struct uui_radio_list *l, int *out_w, int *out_h);
// Positions the control; w/h follow from the grid via
// uui_radio_list_natural_size(), since a radio list cannot be stretched
// into a size its rows and columns don't produce.
void uui_radio_list_set_geometry(struct uui_radio_list *l, int x, int y);

void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l);
// Index under the content-relative point (cx, cy), or -1.
int uui_radio_list_hit(const struct uui_radio_list *l, int cx, int cy);

// Keyboard: the ARROW KEYS move the selection and Space/Enter do
// nothing extra, because on a radio group in Win32, GTK and Qt alike
// ARROWING IS CHOOSING -- there is no separate commit step, and a group
// where the arrows only previewed would be the odd one out. Up/Left go
// back, Down/Right go forward, and the ends do not wrap (Win32 wraps,
// GTK does not; not wrapping is the one that cannot silently jump the
// selection across the whole list on a key repeat).
//
// Returns 1 if the key was consumed AND the selection moved, which is
// also the app's signal to act -- the same event a mouse release
// produces. 0 for any other key, so the app still sees it.
int uui_radio_list_key(struct uui_radio_list *l, int key);

// Full table with ROUTED POINTER INPUT (ui/uui_route.h): selecting an
// option is the widget's job once this is declared, not the app's.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_radio_list_ops;

#endif
