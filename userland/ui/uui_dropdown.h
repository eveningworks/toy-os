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
    uint32_t bg, fg, border;
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
int uui_dropdown_click(struct uui_dropdown *d, int cx, int cy); // 1 if it consumed the click
// A drag inside an OPEN popup, forwarded to its list so the popup's
// scrollbar behaves like any other. No-ops while closed.
int uui_dropdown_drag(struct uui_dropdown *d, int cx, int cy);
void uui_dropdown_drag_end(struct uui_dropdown *d);

int uui_dropdown_key(struct uui_dropdown *d, int key);
int uui_dropdown_selected(const struct uui_dropdown *d);

// Focus-only ops -- see uui_textbox.h's note.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_dropdown_focus_ops;

// Full table with ROUTED POINTER INPUT (ui/uui_route.h), including the
// `overlay_active` slot that makes an OPEN POPUP take every press
// before anything is hit-tested -- the popup is drawn outside this
// widget's own rect, so nothing else can route it correctly.
extern const struct uui_widget_ops uui_dropdown_ops;

#endif
