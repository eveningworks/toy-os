// See ui_primitives.h for the design writeup.
#include "ui_primitives.h"
#include "kapi.h"

int widget_hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

#define BUTTON_PRESSED_NUDGE 1

// Wash strengths, out of 255. Deliberately gentle: the guidelines call
// for feedback that is unmistakable when you look for it and invisible
// when you don't. A hover that shouts is worse than none, because the
// cursor is over SOMETHING at all times.
#define UI_HOVER_ALPHA    22
#define UI_PRESSED_ALPHA  46
#define UI_DISABLED_ALPHA 110

// Which way to shift, decided from the control's own brightness rather
// than assumed.
//
// The first version always lightened for hover, which is the textbook
// description of a hover state and is wrong here: this theme's window
// background is already 235/255, so lightening it moved the pixels by
// TWO -- a hover nobody could see. Measured, not noticed by eye, which
// is the argument for reading pixel values off a screenshot rather than
// looking at one. A light control has to go darker to register, and a
// dark one (a future dark theme, Milestone 19) has to go lighter, so
// the direction follows the base.
static uint32_t shift_from(uint32_t base, uint8_t alpha) {
    uint32_t toward = (gfx_luminance(base) > 128) ? gfx_rgb(0, 0, 0) : gfx_rgb(255, 255, 255);
    return gfx_blend(base, toward, alpha);
}

uint32_t ui_state_bg(uint32_t base, enum ui_state state) {
    switch (state) {
        case UI_STATE_HOVER:    return shift_from(base, UI_HOVER_ALPHA);
        case UI_STATE_PRESSED:  return shift_from(base, UI_PRESSED_ALPHA);
        case UI_STATE_DISABLED: return gfx_blend(base, gfx_rgb(235, 235, 235), UI_DISABLED_ALPHA);
        case UI_STATE_REST:
        default:                return base;
    }
}

void widget_button_state(int x, int y, int w, int h, const char *label,
                          uint32_t bg, uint32_t fg, enum ui_state state) {
    uint32_t fill = ui_state_bg(bg, state);
    gfx_fill_rect(x, y, w, h, fill);
    if (!label) return;

    int lx = x + (w - gfx_text_width(label)) / 2;
    int ly = y + (h - gfx_char_h()) / 2;
    // The nudge is what makes "pressed" feel physical; the darker fill
    // alone reads as a colour change rather than a press.
    if (state == UI_STATE_PRESSED) { lx += BUTTON_PRESSED_NUDGE; ly += BUTTON_PRESSED_NUDGE; }
    gfx_draw_string_clipped(lx, ly, w, label, fg, fill);
}

void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg, int pressed) {
    widget_button_state(x, y, w, h, label, bg, fg, pressed ? UI_STATE_PRESSED : UI_STATE_REST);
}
