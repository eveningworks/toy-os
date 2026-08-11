// See ui_primitives.h for the design writeup.
#include "ui_primitives.h"
#include "kapi.h"

int widget_hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

#define BUTTON_PRESSED_BORDER 2
#define BUTTON_PRESSED_NUDGE 1

void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg, int pressed) {
    gfx_fill_rect(x, y, w, h, bg);
    if (pressed) {
        // A thick-ish inset border rather than a real "darker bg" -- see
        // ui_primitives.h's own comment on why. gfx_rgb(60,60,60) matches
        // THEME_BORDER (theme.h), duplicated here rather than included
        // for it since this file stays independent of the apps-side
        // theme layer -- same reasoning as its other colors always being
        // caller-supplied, never theme-aware itself.
        uint32_t border = gfx_rgb(60, 60, 60);
        for (int i = 0; i < BUTTON_PRESSED_BORDER; i++) {
            gfx_draw_rect(x + i, y + i, w - 2 * i, h - 2 * i, border);
        }
    }
    if (!label) return;

    int len = (int)k_strlen(label);
    int lx = x + (w - len * gfx_char_w()) / 2;
    int ly = y + (h - gfx_char_h()) / 2;
    if (pressed) { lx += BUTTON_PRESSED_NUDGE; ly += BUTTON_PRESSED_NUDGE; }
    gfx_draw_string(lx, ly, label, fg, bg);
}
