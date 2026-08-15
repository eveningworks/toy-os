// A group of buttons, and the press/hover/release rules they share.
// Split out of uui.c -- see ui/uui_button_group.h.
#include "ui/uui_button_group.h"

void uui_button_group_init(struct uui_button_group *g,
                            struct uui_button *buttons, int count) {
    g->buttons = buttons;
    g->count = count;
}

void uui_button_group_draw(const struct uui_button_group *g,
                            struct ugfx_surface *s) {
    for (int i = 0; i < g->count; i++) {
        const struct uui_button *b = &g->buttons[i];
        enum uui_state st = UUI_STATE_REST;
        // Order matters: pressed wins over hovered. A pressed button is
        // always also under the cursor, and showing the hover wash on
        // top of the press would weaken the stronger signal.
        if (b->disabled)     st = UUI_STATE_DISABLED;
        else if (b->pressed) st = UUI_STATE_PRESSED;
        else if (b->hovered) st = UUI_STATE_HOVER;
        uui_button_draw(s, b->x, b->y, b->w, b->h, b->label, b->bg, b->fg, st);
    }
}

int uui_button_group_press(struct uui_button_group *g, int cx, int cy) {
    int changed = 0;
    for (int i = 0; i < g->count; i++) {
        struct uui_button *b = &g->buttons[i];
        int hit = !b->disabled && uui_hit(b->x, b->y, b->w, b->h, cx, cy);
        if (b->pressed != hit) { b->pressed = hit; changed = 1; }
        // A pressed button must not also read as hovered -- the press
        // visual owns the feedback while it lasts.
        if (hit && b->hovered) { b->hovered = 0; changed = 1; }
    }
    return changed;
}

int uui_button_group_hover(struct uui_button_group *g, int cx, int cy) {
    int changed = 0;
    for (int i = 0; i < g->count; i++) {
        struct uui_button *b = &g->buttons[i];
        int hit = !b->disabled && uui_hit(b->x, b->y, b->w, b->h, cx, cy);
        if (b->hovered != hit) { b->hovered = hit; changed = 1; }
    }
    return changed;
}

int uui_button_group_release(struct uui_button_group *g) {
    int code = -1;
    for (int i = 0; i < g->count; i++) {
        struct uui_button *b = &g->buttons[i];
        if (b->pressed) {
            code = b->code;
            b->pressed = 0;
        }
    }
    return code;
}
