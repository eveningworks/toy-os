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

// The EXTENT of the buttons -- the union's own width and height, not
// the distance from the origin to their far edge.
//
// It used to measure from (0,0), which is the same number only while the
// group happens to sit at the origin. That held for as long as nothing
// ever moved a group, and stopped holding the moment one took part in a
// layout: placed at y=284, the group reported a natural HEIGHT of 312,
// which is its offset plus its size. In a column that inflated the space
// the layout believed its children needed, so the widget above it (Task
// Manager's table) grew by 16 px instead of the 300 it was given -- a
// resize that looked like the table simply ignoring the window.
//
// A natural size that depends on where the widget currently IS cannot be
// right: it is the size the widget wants, asked before anyone knows
// where it goes.
void uui_button_group_natural_size(const struct uui_button_group *g,
                                    int *out_w, int *out_h) {
    if (g->count <= 0) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    int x0 = g->buttons[0].x, y0 = g->buttons[0].y;
    int x1 = x0 + g->buttons[0].w, y1 = y0 + g->buttons[0].h;
    for (int i = 1; i < g->count; i++) {
        const struct uui_button *b = &g->buttons[i];
        if (b->x < x0) x0 = b->x;
        if (b->y < y0) y0 = b->y;
        if (b->x + b->w > x1) x1 = b->x + b->w;
        if (b->y + b->h > y1) y1 = b->y + b->h;
    }
    if (out_w) *out_w = x1 - x0;
    if (out_h) *out_h = y1 - y0;
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

static int bg_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
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

// --- taking part in a layout -----------------------------------------
//
// These two were missing, and the symptom was not a compile error: a
// group declared in a layout kept whatever coordinates its buttons were
// given at init and was allotted no space, so the buttons drew wherever
// they happened to sit -- on top of the widget above them -- while the
// layout believed the group occupied nothing. Every slot in
// uui_widget_ops is optional, which is the right default and also means
// an omission like this is silent.
static void bg_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_button_group_natural_size((const struct uui_button_group *)w, out_w, out_h);
}

// TRANSLATES the buttons rather than re-laying them out: their relative
// arrangement is the app's (a row, a grid, a gap between two of them),
// and a group that re-flowed them here would silently override it.
static void bg_ops_geometry(void *w, int x, int y, int width, int height) {
    (void)width; (void)height;
    struct uui_button_group *g = (struct uui_button_group *)w;
    if (g->count <= 0) return;

    // The group's origin is the top-left of the union of its buttons,
    // which is what natural_size() reported -- so shifting by the
    // difference puts that union exactly where the layout asked.
    int min_x = g->buttons[0].x, min_y = g->buttons[0].y;
    for (int i = 1; i < g->count; i++) {
        if (g->buttons[i].x < min_x) min_x = g->buttons[i].x;
        if (g->buttons[i].y < min_y) min_y = g->buttons[i].y;
    }
    int dx = x - min_x, dy = y - min_y;
    for (int i = 0; i < g->count; i++) {
        g->buttons[i].x += dx;
        g->buttons[i].y += dy;
    }
}

const struct uui_widget_ops uui_button_group_ops = {
    .natural_size = bg_ops_natural_size,
    .set_geometry = bg_ops_geometry,
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
