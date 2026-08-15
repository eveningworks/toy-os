// See ui_button.h for the design writeup (Brutal-OS-inspired owned-state
// button object, sized down for toy-os's immediate-mode GUI).
#include "ui_button.h"
#include "ui_primitives.h"
#include "kapi.h" // gfx_rgb() -- disabled-state dimming below

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
    b->hovered = 0;
    b->disabled = 0;
}

void ui_button_natural_size(const struct ui_button *b, int *out_w, int *out_h) {
    if (out_w) *out_w = gfx_text_width(b->label) + 2 * UI_PAD_X;
    if (out_h) *out_h = gfx_char_h() + 2 * UI_PAD_Y;
}

void ui_button_set_geometry(struct ui_button *b, int x, int y, int w, int h) {
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
}

void ui_button_set_disabled(struct ui_button *b, int disabled) {
    b->disabled = disabled;
}

void ui_button_draw(const struct ui_button *b, int origin_x, int origin_y) {
    uint32_t bg = b->bg, fg = b->fg;
    // Pressed outranks hovered, and a press clears `hovered` anyway
    // (ui_button_group_press()) -- the belt-and-braces order here is so
    // a caller driving the flags by hand can't produce a button that's
    // held down and drawn as merely hovered.
    enum ui_state state = b->pressed ? UI_STATE_PRESSED
                        : b->hovered ? UI_STATE_HOVER
                                     : UI_STATE_REST;
    if (b->disabled) {
        // Flat gray regardless of the caller's own bg/fg -- same
        // hardcoded, theme-agnostic precedent as widget_button()'s own
        // pressed-border color (ui_primitives.c); this file stays
        // independent of apps/theme.h same as that one does.
        bg = gfx_rgb(210, 210, 210);
        fg = gfx_rgb(140, 140, 140);
        // Already its own flat gray -- no wash on top, and neither
        // press nor hover should read on a control that does nothing.
        // (This is deliberately NOT UI_STATE_DISABLED: that muting is
        // derived from the caller's own colour, and this branch has
        // already replaced it. Switching to it would change how every
        // disabled button looks, which isn't what this change is.)
        state = UI_STATE_REST;
    }
    widget_button_state(origin_x + b->x, origin_y + b->y, b->w, b->h, b->label, bg, fg, state);
}

int ui_button_hit(const struct ui_button *b, int cx, int cy) {
    return widget_hit(b->x, b->y, b->w, b->h, cx, cy);
}
