// A group of buttons, and the press/hover/release rules they share.
// Split out of uui.c -- see ui/uui_button_group.h.
#include "ui/uui_button_group.h"
#include "ui/uui_widget.h"     // the ops table the router dispatches through

void uui_button_group_init(struct uui_button_group *g,
                            struct uui_button *buttons, int count) {
    g->buttons = buttons;
    g->count = count;
    g->activated = 0;
}

void uui_button_group_natural_size(const struct uui_button_group *g,
                                    int *out_w, int *out_h) {
    int x1 = 0, y1 = 0;
    for (int i = 0; i < g->count; i++) {
        const struct uui_button *b = &g->buttons[i];
        if (b->x + b->w > x1) x1 = b->x + b->w;
        if (b->y + b->h > y1) y1 = b->y + b->h;
    }
    if (out_w) *out_w = x1;
    if (out_h) *out_h = y1;
}

void uui_button_group_draw(const struct uui_button_group *g,
                            struct ugfx_surface *s) {
    for (int i = 0; i < g->count; i++) {
        uui_button_draw_one(s, &g->buttons[i]);
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

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// **The one control where commit-on-release earns its keep.** A press
// arms; the release commits only if the cursor is still on the button
// it armed. The pointer GRAB is what makes that work: without it, a
// press dragged off the group would never deliver a release here at
// all, and the arm would be left set.
//
// `activated` holds the committed code until the app collects it with
// uui_button_group_take_activated(), because the router reports WHICH
// WIDGET changed, not what it decided -- a group has many buttons and
// one id.
static int bg_ops_hit(const void *w, int cx, int cy) {
    const struct uui_button_group *g = (const struct uui_button_group *)w;
    for (int i = 0; i < g->count; i++) {
        if (uui_hit(g->buttons[i].x, g->buttons[i].y,
                    g->buttons[i].w, g->buttons[i].h, cx, cy)) return 1;
    }
    return 0;
}

static int bg_ops_press(void *w, int cx, int cy) {
    struct uui_button_group *g = (struct uui_button_group *)w;
    uui_button_group_press(g, cx, cy);
    // Consumed whether or not a button lit up: the press landed inside
    // the group's bounds, and letting it fall through to a widget
    // underneath would be worse than doing nothing.
    return 1;
}

static int bg_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_button_group *g = (struct uui_button_group *)w;
    // Held: re-arm against the CURRENT position, so dragging off
    // disarms and dragging back re-arms, as every real toolkit does.
    if (buttons) return uui_button_group_press(g, cx, cy);
    return uui_button_group_hover(g, cx, cy);
}

static int bg_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_button_group *g = (struct uui_button_group *)w;
    int code = uui_button_group_release(g);
    if (code > 0) g->activated = code;
    return 1;
}

static void bg_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_button_group_draw((const struct uui_button_group *)w, s);
}

const struct uui_widget_ops uui_button_group_ops = {
    .draw    = bg_ops_draw,
    .hit     = bg_ops_hit,
    .press   = bg_ops_press,
    .motion  = bg_ops_motion,
    .release = bg_ops_release,
};

int uui_button_group_take_activated(struct uui_button_group *g) {
    int code = g->activated;
    g->activated = 0;
    return code;
}
