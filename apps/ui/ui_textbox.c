// See ui_textbox.h for the design writeup.
#include "ui_textbox.h"

void ui_textbox_init(struct ui_textbox *tbx, int x, int y, int w, int h,
                      const char *initial, uint32_t bg, uint32_t fg, uint32_t border) {
    tbx->x = x;
    tbx->y = y;
    tbx->w = w;
    tbx->h = h;
    tbx->bg = bg;
    tbx->fg = fg;
    tbx->border = border;
    widget_textfield_init(&tbx->field, initial);
}

void ui_textbox_set_geometry(struct ui_textbox *tbx, int x, int y, int w, int h) {
    tbx->x = x;
    tbx->y = y;
    tbx->w = w;
    tbx->h = h;
}

void ui_textbox_draw(const struct ui_textbox *tbx, int origin_x, int origin_y) {
    widget_textfield_draw(origin_x + tbx->x, origin_y + tbx->y, tbx->w, tbx->h,
                           &tbx->field, tbx->bg, tbx->fg, tbx->border);
}

int ui_textbox_hit(const struct ui_textbox *tbx, int cx, int cy) {
    return widget_hit(tbx->x, tbx->y, tbx->w, tbx->h, cx, cy);
}

void ui_textbox_set_active(struct ui_textbox *tbx, int active) {
    widget_textfield_set_active(&tbx->field, active);
}

int ui_textbox_key(struct ui_textbox *tbx, int key) {
    return widget_textfield_key(&tbx->field, key);
}
