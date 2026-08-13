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
        // The press visual owns the feedback while anything is held --
        // docs/gui-guidelines.md. The WM stops delivering on_hover for
        // the duration of a press, so without this a button that was
        // hovered when the press started would keep its hover flag set
        // underneath, and re-appear as hovered the moment the press
        // moved off it.
        if (g->buttons[i].hovered) {
            g->buttons[i].hovered = 0;
            changed = 1;
        }
    }
    return changed;
}

int ui_button_group_hover(struct ui_button_group *g, int cx, int cy) {
    int hit = hit_index(g, cx, cy);
    int changed = 0;
    for (int i = 0; i < g->count; i++) {
        int should = (i == hit);
        if (g->buttons[i].hovered != should) {
            g->buttons[i].hovered = should;
            changed = 1;
        }
    }
    return changed;
}

int ui_button_group_release(struct ui_button_group *g) {
    int code = -1;
    for (int i = 0; i < g->count; i++) {
        // At most one button is ever pressed (ui_button_group_press()
        // maintains that), and it's only still pressed if the cursor
        // was over it on the last tick -- so this IS "released over the
        // armed control", the condition the commit is gated on.
        if (g->buttons[i].pressed) code = g->buttons[i].code;
        g->buttons[i].pressed = 0;
    }
    return code;
}

// (No ui_button_group_click() here any more -- see the header.)
