// See ui_button.h for the design writeup (Brutal-OS-inspired owned-state
// button object, sized down for toy-os's immediate-mode GUI).
#include "ui_button.h"
#include "ui_primitives.h"

void ui_button_init(struct ui_button *b, int x, int y, int w, int h,
                     const char *label, uint32_t bg, uint32_t fg, int code) {
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
    b->label = label;
    b->bg = bg;
    b->fg = fg;
    b->code = code;
    b->pressed = 0;
}

void ui_button_set_geometry(struct ui_button *b, int x, int y, int w, int h) {
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
}

void ui_button_draw(const struct ui_button *b, int origin_x, int origin_y) {
    widget_button(origin_x + b->x, origin_y + b->y, b->w, b->h, b->label, b->bg, b->fg, b->pressed);
}

int ui_button_hit(const struct ui_button *b, int cx, int cy) {
    return widget_hit(b->x, b->y, b->w, b->h, cx, cy);
}
