#ifndef UUI_DROPDOWN_H
#define UUI_DROPDOWN_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_listbox.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- dropdown ---------------------------------------------------------

struct uui_dropdown {
    int x, y, w, h;
    struct uui_listbox list; // the popup, which IS a listbox
    int open;
    int max_rows;
    int focused;  // OWNED -- driven by the focus ring's set_focused
    uint32_t bg, fg, border;

    // VISIBLE, BUT DOES NOTHING -- the same field uui_button and
    // uui_checkbox already carry. Set it and the control draws dimmed
    // and refuses press/motion/release/key, and drops out of the focus
    // ring so Tab does not stop on something that cannot be used.
    //
    // A DISABLED CONTROL IS ONLY AN IMPROVEMENT WITH A REASON BESIDE
    // IT. Nothing here draws that reason -- the widget has nowhere to
    // put it -- so whoever sets this owes the user a sentence (System
    // Settings prints the registry's `unavailable` text above the
    // control; see abi/setting_abi.h).
    int disabled;

    // The popup's own compositor surface, or 0 when it is drawn
    // in-window. OWNED; every open/close goes through dd_open/dd_close
    // so the surface cannot outlive the `open` flag.
    int popup;
    // A press that can end in a choice: one in the list, or the one that
    // opened it (press on the box, drag into the list, release). The
    // choice is made on RELEASE -- a press-commit closed the popup under
    // its own release, which then reached nobody.
    int tracking;
};

void uui_dropdown_init(struct uui_dropdown *d, int x, int y, int w, int h,
                        const char *const *items, int count);
// Moves the box and re-places the popup under it. Setting x/y/w/h by
// hand leaves the popup where init() put it -- see the .c file.
void uui_dropdown_set_geometry(struct uui_dropdown *d, int x, int y, int w, int h);

// Preferred minimum for the CLOSED box: the widest item it could show,
// plus insets and arrow room. Widest ITEM, not the selected one -- a box
// that fits today's value and clips tomorrow's is the bug this
// prevents. See uui_primitives.h.
void uui_dropdown_natural_size(const struct uui_dropdown *d, int *out_w, int *out_h);

void uui_dropdown_draw(struct ugfx_surface *s, const struct uui_dropdown *d);

// **Call this AFTER every other widget has drawn.** Drawing is
// immediate-mode, so z-order is call order -- a popup drawn in place
// would be painted over by whatever comes next. Same rule the kernel
// dropdown documents, and the same bug if ignored.
void uui_dropdown_draw_popup(struct ugfx_surface *s, const struct uui_dropdown *d);

int uui_dropdown_hit(const struct uui_dropdown *d, int cx, int cy);
// The PRESS: opens or closes on the box, scrolls on the popup's bar,
// ARMS a row in the list, dismisses elsewhere. 1 if it consumed it.
int uui_dropdown_click(struct uui_dropdown *d, int cx, int cy);
// A held drag: the popup's scrollbar thumb, or the row a release would
// pick. No-ops while closed.
int uui_dropdown_drag(struct uui_dropdown *d, int cx, int cy);
// The RELEASE, where a row is chosen -- as a Windows, Qt or GTK combo
// box chooses -- and the popup closes. 1 if the selection changed.
int uui_dropdown_release(struct uui_dropdown *d, int cx, int cy);
void uui_dropdown_drag_end(struct uui_dropdown *d);

// While OPEN the popup takes everything (Esc dismisses, Enter commits,
// the rest goes to the list). While CLOSED it takes only the keys that
// OPEN it -- Down, Enter, Space -- plus a PRINTABLE key, which seeks
// the value in place the way a Windows or KDE combobox does. The
// arrows, Home and End are still ignored while closed, deliberately: a
// value must not walk where the user cannot see it.
int uui_dropdown_key(struct uui_dropdown *d, int key);
int uui_dropdown_selected(const struct uui_dropdown *d);
// The other half of the pair. Clamped to the item range, and -1 selects
// nothing. For a caller whose item LIST is rebuilt while the control is
// live -- re-initialising resets the selection to the first item, which
// silently changes what the user was looking at.
void uui_dropdown_set_selected(struct uui_dropdown *d, int index);

// REPLACE THE ITEMS WITHOUT RE-INITIALISING. init() also resets x/y/w/h
// to what it was passed, so a caller that re-inits to change the list
// hands the widget a ZERO RECT -- and a layout that only assigns
// geometry when something resizes never puts it back, so the control
// simply stops being drawn. The selection is clamped, not reset; pair
// this with set_selected() to restore one by name.
void uui_dropdown_set_items(struct uui_dropdown *d, const char *const *items, int count);

// Focus-only ops -- see uui_textbox.h's note.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_dropdown_focus_ops;

// Full table with ROUTED POINTER INPUT (ui/uui_route.h), including the
// `overlay_active` slot that makes an OPEN POPUP take every press
// before anything is hit-tested -- the popup is drawn outside this
// widget's own rect, so nothing else can route it correctly.
extern const struct uui_widget_ops uui_dropdown_ops;

#endif
