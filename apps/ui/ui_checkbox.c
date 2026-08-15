// See ui_checkbox.h for the design writeup.
#include "ui_checkbox.h"
#include "ui_primitives.h" // widget_hit()
#include "kapi.h"

#define CHECKBOX_LABEL_GAP 6

void widget_checkbox_natural_size(int size, const char *label, int *out_w, int *out_h) {
    if (out_w) *out_w = label ? size + CHECKBOX_LABEL_GAP + gfx_text_width(label) : size;
    // The box or the text, whichever is taller -- a checkbox drawn at a
    // small `size` next to a large font must not clip its own label.
    if (out_h) *out_h = size > gfx_char_h() ? size : gfx_char_h();
}

void widget_checkbox_draw(int x, int y, int size, int checked, int hovered,
                           const char *label, uint32_t bg, uint32_t fg) {
    if (hovered) {
        // The whole clickable area, not just the box -- the hit test is
        // box+label (see widget_checkbox_hit), and a highlight smaller
        // than the target it describes is a lie about where to click.
        int hw, hh;
        widget_checkbox_natural_size(size, label, &hw, &hh);
        bg = ui_state_bg(bg, UI_STATE_HOVER);
        gfx_fill_rect(x, y, hw, hh, bg);
    }
    gfx_draw_rect(x, y, size, size, fg);
    if (checked) {
        int inset = size / 4 > 0 ? size / 4 : 1;
        gfx_fill_rect(x + inset, y + inset, size - 2 * inset, size - 2 * inset, fg);
    }
    if (label) {
        int row_h = gfx_char_h();
        int label_y = y + (size - row_h) / 2;
        gfx_draw_string(x + size + CHECKBOX_LABEL_GAP, label_y, label, fg, bg);
    }
}

int widget_checkbox_hit(int x, int y, int size, const char *label, int px, int py) {
    // The same box the draw highlights and the same one a layout would
    // reserve -- one calculation, three users. The hover wash, the hit
    // test and the natural size disagreeing is the classic version of
    // this bug (see ui_scrollbar.c's shared geometry helper).
    int w, h;
    widget_checkbox_natural_size(size, label, &w, &h);
    return widget_hit(x, y, w, h, px, py);
}
