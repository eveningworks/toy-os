// See ui_checkbox.h for the design writeup.
#include "ui_checkbox.h"
#include "ui_primitives.h" // widget_hit()
#include "kapi.h"

#define CHECKBOX_LABEL_GAP 6

int widget_checkbox_width(int size, const char *label) {
    if (!label) return size;
    return size + CHECKBOX_LABEL_GAP + (int)k_strlen(label) * gfx_char_w();
}

void widget_checkbox_draw(int x, int y, int size, int checked, const char *label,
                           uint32_t bg, uint32_t fg) {
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
    int w = widget_checkbox_width(size, label);
    int row_h = gfx_char_h();
    int h = size > row_h ? size : row_h;
    return widget_hit(x, y, w, h, px, py);
}
