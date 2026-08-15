// checkbox. Split out of uwidgets.c -- see ui/uui_checkbox.h.
#include "ui/uui_checkbox.h"
#include "string.h"   // k_strlen -- the shared one, see CLAUDE.md

// ---------------------------------------------------------------------
// checkbox
// ---------------------------------------------------------------------

#define CHECKBOX_LABEL_GAP 6

void uui_checkbox_natural_size(int size, const char *label, int *out_w, int *out_h) {
    if (out_w) *out_w = label ? size + CHECKBOX_LABEL_GAP + ugfx_text_width(label) : size;
    // The box or the text, whichever is taller.
    if (out_h) *out_h = size > ugfx_char_h() ? size : ugfx_char_h();
}

void uui_checkbox_draw(struct ugfx_surface *s, int x, int y, int size,
                        int checked, int hovered, const char *label,
                        uint32_t bg, uint32_t fg) {
    if (hovered) {
        // The WHOLE clickable area, because that is what the hit test
        // covers -- a highlight smaller than its target misleads about
        // where to click.
        int hw, hh;
        uui_checkbox_natural_size(size, label, &hw, &hh);
        bg = uui_state_bg(bg, UUI_STATE_HOVER);
        ugfx_fill_rect(s, x, y, hw, hh, bg);
    }
    ugfx_draw_rect(s, x, y, size, size, fg);
    if (checked) {
        int inset = size / 4 > 0 ? size / 4 : 1;
        ugfx_fill_rect(s, x + inset, y + inset, size - 2 * inset, size - 2 * inset, fg);
    }
    if (label) {
        ugfx_draw_string(s, x + size + CHECKBOX_LABEL_GAP,
                          y + (size - ugfx_char_h()) / 2, label, fg, bg);
    }
}

int uui_checkbox_hit(int x, int y, int size, const char *label, int px, int py) {
    // The same box the draw highlights and a layout would reserve --
    // one calculation, three users.
    int w, h;
    uui_checkbox_natural_size(size, label, &w, &h);
    return uui_hit(x, y, w, h, px, py);
}
