// See ui_button_group.h for the design writeup.
#include "ui_button_group.h"

void ui_button_group_init(struct ui_button_group *g, struct ui_button *buttons, int count) {
    g->buttons = buttons;
    g->count = count;
}

void ui_button_group_draw(const struct ui_button_group *g, int origin_x, int origin_y) {
    for (int i = 0; i < g->count; i++) {
        ui_button_draw(&g->buttons[i], origin_x, origin_y);
    }
}

static int hit_index(const struct ui_button_group *g, int cx, int cy) {
    for (int i = 0; i < g->count; i++) {
        if (g->buttons[i].disabled) continue; // non-interactive -- see ui_button_set_disabled()
        if (ui_button_hit(&g->buttons[i], cx, cy)) return i;
    }
    return -1;
}

int ui_button_group_press(struct ui_button_group *g, int cx, int cy) {
    int hit = hit_index(g, cx, cy);
    int changed = 0;
    for (int i = 0; i < g->count; i++) {
        int should = (i == hit);
        if (g->buttons[i].pressed != should) {
            g->buttons[i].pressed = should;
            changed = 1;
        }
    }
    return changed;
}

void ui_button_group_release(struct ui_button_group *g) {
    for (int i = 0; i < g->count; i++) {
        g->buttons[i].pressed = 0;
    }
}

int ui_button_group_click(struct ui_button_group *g, int cx, int cy) {
    int hit = hit_index(g, cx, cy);
    if (hit < 0) return -1;
    return g->buttons[hit].code;
}
