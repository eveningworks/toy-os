// See ui_button_group.h for the design writeup.
#include "kapi.h" // KEY_ARROW_* for the arrow navigation below
#include "ui_focus.h"
#include "ui_button_group.h"

void ui_button_group_init(struct ui_button_group *g, struct ui_button *buttons, int count) {
    g->buttons = buttons;
    g->count = count;
    g->focus_index = -1;
    g->activated = -1;
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

// ---- focus integration ----------------------------------------------
//
// See ui_focus.h. A group is ONE focus stop, not one per button --
// arrows move between buttons within it, which is how a real toolkit
// treats a related row of controls and keeps the tab ring short.
//
// The focused button is tracked here as `focus_index` rather than in
// struct ui_button, because it is a property of the group's navigation,
// not of any one button: only one can hold it, and ui_button already has
// two owned flags whose invariants a third would complicate.
static int group_first_enabled(const struct ui_button_group *g, int from, int dir) {
    for (int n = 0; n < g->count; n++) {
        int i = ((from + dir * n) % g->count + g->count) % g->count;
        if (!g->buttons[i].disabled) return i;
    }
    return -1;
}

static int focus_key(void *w, int key, uint8_t mods) {
    (void)mods;
    struct ui_button_group *g = (struct ui_button_group *)w;
    if (g->count <= 0) return 0;

    if (key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT) {
        int dir = (key == KEY_ARROW_RIGHT) ? 1 : -1;
        int start = g->focus_index < 0 ? 0 : g->focus_index + dir;
        int next = group_first_enabled(g, start, dir);
        if (next < 0) return 0;
        g->focus_index = next;
        return 1;
    }
    // Space and Enter activate, the two keys every toolkit binds to "the
    // focused button". Reported through the same code path a mouse
    // release uses, so an app's on_release logic handles both.
    if (key == ' ' || key == '\n' || key == '\r') {
        if (g->focus_index < 0 || g->focus_index >= g->count) return 0;
        if (g->buttons[g->focus_index].disabled) return 0;
        g->activated = g->buttons[g->focus_index].code;
        return 1;
    }
    return 0;
}

static int focus_hit(const void *w, int cx, int cy) {
    const struct ui_button_group *g = (const struct ui_button_group *)w;
    for (int i = 0; i < g->count; i++) {
        if (g->buttons[i].disabled) continue;
        if (ui_button_hit(&g->buttons[i], cx, cy)) return 1;
    }
    return 0;
}

static void focus_ring(const void *w, int ox, int oy, uint32_t color) {
    const struct ui_button_group *g = (const struct ui_button_group *)w;
    if (g->focus_index < 0 || g->focus_index >= g->count) return;
    const struct ui_button *b = &g->buttons[g->focus_index];
    ui_focus_ring_rect(b->x, b->y, b->w, b->h, ox, oy, color);
}

static int focus_accepts(const void *w) {
    const struct ui_button_group *g = (const struct ui_button_group *)w;
    return group_first_enabled(g, 0, 1) >= 0;
}

// Arriving focus lands on the first enabled button, so the very first
// arrow keypress moves from a real starting point rather than being
// swallowed establishing one.
static void focus_set(void *w, int focused) {
    struct ui_button_group *g = (struct ui_button_group *)w;
    if (focused) {
        if (g->focus_index < 0) g->focus_index = group_first_enabled(g, 0, 1);
    } else {
        g->focus_index = -1;
    }
}

const struct ui_focus_ops ui_button_group_focus_ops = {
    .key = focus_key,
    .hit = focus_hit,
    .draw_ring = focus_ring,
    .accepts_focus = focus_accepts,
    .set_focused = focus_set,
};

int ui_button_group_take_activated(struct ui_button_group *g) {
    int code = g->activated;
    g->activated = -1;
    return code;
}
