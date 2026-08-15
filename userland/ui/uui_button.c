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

void uui_button_draw_one(struct ugfx_surface *s, const struct uui_button *b) {
    // Order matters: pressed wins over hovered. A pressed button is
    // always also under the cursor, and showing the hover wash on top
    // of the press would weaken the stronger signal.
    enum uui_state st = UUI_STATE_REST;
    if (b->disabled)     st = UUI_STATE_DISABLED;
    else if (b->pressed) st = UUI_STATE_PRESSED;
    else if (b->hovered) st = UUI_STATE_HOVER;
    uui_button_draw(s, b->x, b->y, b->w, b->h, b->label, b->bg, b->fg, st);
}

// --- as a layout child ------------------------------------------------
//
// This USED to have no `draw` slot, on the theory that a button is
// painted by its GROUP and the layout only places it. That was wrong,
// and wrong in the worst way: Calculator's twenty buttons were placed,
// hit-tested and never drawn. It shipped, because every test asserted
// that clicking a button CHANGED THE DISPLAY -- which it did. "A click
// has an effect" is not "the button is visible", and nothing was
// checking the second.

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

static void btn_draw(struct ugfx_surface *s, const void *w) {
    uui_button_draw_one(s, (const struct uui_button *)w);
}

const struct uui_widget_ops uui_button_ops = {
    .natural_size = btn_natural,
    .set_geometry = btn_geometry,
    .draw         = btn_draw,
    .hit          = btn_hit,
};
