// A single button's state. Split out of uui.c -- see ui/uui_button.h.
#include "ui/uui_button.h"

void uui_button_init(struct uui_button *b, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg, int code) {
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->label = label;
    b->bg = bg; b->fg = fg;
    b->code = code;
    b->pressed = 0;
    b->hovered = 0;
    b->disabled = 0;
}

void uui_button_natural_size(const struct uui_button *b, int *out_w, int *out_h) {
    if (out_w) *out_w = ugfx_text_width(b->label) + 2 * UUI_PAD_X;
    if (out_h) *out_h = ugfx_char_h() + 2 * UUI_PAD_Y;
}

void uui_button_set_geometry(struct uui_button *b, int x, int y, int w, int h) {
    b->x = x; b->y = y; b->w = w; b->h = h;
}
