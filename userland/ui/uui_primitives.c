// Interaction states and the button painter. Split out of uui.c --
// see ui/uui_primitives.h.
#include "ui/uui_primitives.h"
#include "ui/utheme.h"

#define UUI_PRESSED_NUDGE 1

// Wash strengths out of 255, carried over from apps/ui/ui_primitives.c
// unchanged so a client's controls feel identical to the desktop's.
// Deliberately gentle: the guidelines want feedback that is
// unmistakable when you look for it and invisible when you don't -- a
// hover that shouts is worse than none, because the cursor is over
// SOMETHING at all times.
#define UUI_HOVER_ALPHA    22
#define UUI_PRESSED_ALPHA  46
#define UUI_DISABLED_ALPHA 110

// Which way to shift, read from the control's own brightness rather
// than assumed. A light control has to go DARKER to register; only a
// dark one goes lighter. See uui.h's header comment for the bug that
// this exists to prevent.
static uint32_t shift_from(uint32_t base, uint8_t alpha) {
    uint32_t toward = (ugfx_luminance(base) > 128) ? ugfx_rgb(0, 0, 0)
                                                    : ugfx_rgb(255, 255, 255);
    return ugfx_blend(base, toward, alpha);
}

uint32_t uui_state_bg(uint32_t base, enum uui_state state) {
    switch (state) {
    case UUI_STATE_HOVER:    return shift_from(base, UUI_HOVER_ALPHA);
    case UUI_STATE_PRESSED:  return shift_from(base, UUI_PRESSED_ALPHA);
    case UUI_STATE_DISABLED: return ugfx_blend(base, ugfx_rgb(235, 235, 235),
                                                UUI_DISABLED_ALPHA);
    case UUI_STATE_REST:
    default:                 return base;
    }
}

int uui_hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void uui_button_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg,
                      enum uui_state state) {
    uint32_t fill = uui_state_bg(bg, state);
    ugfx_fill_rect(s, x, y, w, h, fill);
    if (!label) return;

    int lx = x + (w - ugfx_text_width(label)) / 2;
    int ly = y + (h - ugfx_char_h()) / 2;
    // The nudge is what makes a press feel physical; the darker fill on
    // its own reads as a colour change.
    if (state == UUI_STATE_PRESSED) { lx += UUI_PRESSED_NUDGE; ly += UUI_PRESSED_NUDGE; }
    ugfx_draw_string_clipped(s, lx, ly, w, label, fg, fill);
}

void uui_focus_ring(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    ugfx_draw_rect(s, x, y, w, h, UTHEME_ACCENT);
}
