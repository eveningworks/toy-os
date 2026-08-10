#include "widgets.h"
#include "kapi.h"

int widget_hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg) {
    gfx_fill_rect(x, y, w, h, bg);
    if (!label) return;

    int len = (int)k_strlen(label);
    int lx = x + (w - len * gfx_char_w()) / 2;
    int ly = y + (h - gfx_char_h()) / 2;
    gfx_draw_string(lx, ly, label, fg, bg);
}
