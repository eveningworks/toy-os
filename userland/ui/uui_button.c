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

// --- as a layout child ------------------------------------------------
//
// No `draw` slot: a button is drawn by its GROUP (uui_button_group),
// which is what knows the pressed/hovered state to draw it in. The
// layout places buttons; the group paints them. Two owners of two
// different things, rather than one struct pretending to be both.

static void btn_natural(const void *w, int *out_w, int *out_h) {
    uui_button_natural_size((const struct uui_button *)w, out_w, out_h);
}
static void btn_geometry(void *w, int x, int y, int width, int height) {
    uui_button_set_geometry((struct uui_button *)w, x, y, width, height);
}
static int btn_hit(const void *w, int cx, int cy) {
    const struct uui_button *b = (const struct uui_button *)w;
    return uui_hit(b->x, b->y, b->w, b->h, cx, cy);
}

const struct uui_widget_ops uui_button_ops = {
    .natural_size = btn_natural,
    .set_geometry = btn_geometry,
    .hit          = btn_hit,
};
