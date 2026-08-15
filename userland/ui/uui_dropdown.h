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
void uui_dropdown_draw(struct ugfx_surface *s, const struct uui_dropdown *d);

// **Call this AFTER every other widget has drawn.** Drawing is
// immediate-mode, so z-order is call order -- a popup drawn in place
// would be painted over by whatever comes next. Same rule the kernel
// dropdown documents, and the same bug if ignored.
void uui_dropdown_draw_popup(struct ugfx_surface *s, const struct uui_dropdown *d);

int uui_dropdown_hit(const struct uui_dropdown *d, int cx, int cy);
int uui_dropdown_click(struct uui_dropdown *d, int cx, int cy); // 1 if it consumed the click
int uui_dropdown_key(struct uui_dropdown *d, int key);
int uui_dropdown_selected(const struct uui_dropdown *d);

#endif
