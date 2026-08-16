#ifndef UUI_LISTBOX_H
#define UUI_LISTBOX_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- listbox ----------------------------------------------------------

struct uui_listbox {
    int x, y, w, h;
    const char *const *items; // caller-owned
    int count;
    int selected;  // or -1
    int hovered;   // OWNED -- driven by uui_listbox_hover()
    int top;       // first visible row; OWNED
    int row_h;     // 0 = derive from the font
    int bar_w;
    // Live thumb-drag state: -1 when no drag is in progress, otherwise
    // the offset WITHIN the thumb that was grabbed. OWNED -- driven by
    // uui_listbox_press()/_drag()/_drag_end().
    int thumb_grab;
    uint32_t bg, fg, sel_bg, sel_fg, track_bg, thumb_bg;
};

void uui_listbox_init(struct uui_listbox *lb, int x, int y, int w, int h,
                       const char *const *items, int count);
void uui_listbox_set_items(struct uui_listbox *lb, const char *const *items, int count);
int  uui_listbox_row_h(const struct uui_listbox *lb);
int  uui_listbox_visible_rows(const struct uui_listbox *lb);
int  uui_listbox_scrollbar_visible(const struct uui_listbox *lb);
void uui_listbox_draw(struct ugfx_surface *s, const struct uui_listbox *lb);

// Row index at (cx, cy), or -1 if outside / on the scrollbar.
// Preferred minimum: wide enough for the longest item plus insets and
// the scrollbar, tall enough for every row. A layout may give LESS
// height and the listbox scrolls, which is what it is for. See
// uui_primitives.h.
void uui_listbox_natural_size(const struct uui_listbox *lb, int *out_w, int *out_h);

int  uui_listbox_hit(const struct uui_listbox *lb, int cx, int cy);
int  uui_listbox_hover(struct uui_listbox *lb, int cx, int cy);   // 1 if changed
int  uui_listbox_click(struct uui_listbox *lb, int cx, int cy);   // selects; 1 if changed
int  uui_listbox_wheel(struct uui_listbox *lb, int notches);      // 1 if scrolled

// --- the scrollbar, which is the widget's job and not the app's ------
//
// A press on the strip: starts a thumb drag, or pages when it lands on
// the track. Returns 1 if it claimed the press, so an app can fall
// through to selection when it did not. Forward press/drag/drag_end and
// the bar simply works -- a listbox that drew a bar and handled none of
// this shipped once already, and a user found it by dragging.
int  uui_listbox_press(struct uui_listbox *lb, int cx, int cy);
int  uui_listbox_drag(struct uui_listbox *lb, int cx, int cy);    // 1 if scrolled
void uui_listbox_drag_end(struct uui_listbox *lb);
int  uui_listbox_key(struct uui_listbox *lb, int key);            // arrows/home/end

// Focus-only ops -- see uui_textbox.h's note.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_listbox_focus_ops;

// The full table, including ROUTED POINTER INPUT (ui/uui_route.h): an
// app that declares a listbox with this needs no input code at all --
// selection, scrollbar paging and thumb dragging are the widget's.
extern const struct uui_widget_ops uui_listbox_ops;

#endif
