#ifndef UUI_SCROLLBAR_H
#define UUI_SCROLLBAR_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- scrollbar --------------------------------------------------------

#define UUI_SCROLLBAR_MIN_THUMB_H 16

enum uui_scrollbar_zone {
    UUI_SB_NONE = 0,
    UUI_SB_ABOVE,  // the track above the thumb -- page up
    UUI_SB_THUMB,  // the thumb itself -- start a drag
    UUI_SB_BELOW,  // the track below the thumb -- page down
};

void uui_scrollbar_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                         int total_lines, int visible_rows, int scroll_offset,
                         uint32_t track_bg, uint32_t thumb_bg);

enum uui_scrollbar_zone uui_scrollbar_hit(int x, int y, int w, int h,
                                           int total_lines, int visible_rows,
                                           int scroll_offset, int px, int py);

void uui_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                               int scroll_offset, int *out_thumb_y, int *out_thumb_h);

// The scroll offset a thumb drag to `py` implies. `grab_offset_in_thumb`
// is how far down the thumb the drag started, so the thumb doesn't jump
// under the cursor on the first pixel of movement.
int uui_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                   int py, int grab_offset_in_thumb);

#endif
