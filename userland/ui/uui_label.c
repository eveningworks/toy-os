// label -- see ui/uui_label.h for why this exists at all.
#include "ui/uui_label.h"
#include "ui/uui_widget.h"
#include <stddef.h>

void uui_label_init(struct uui_label *l, const char *text) {
    l->x = l->y = l->w = l->h = 0;
    l->text = text;
    l->fg = ugfx_rgb(20, 20, 20);
    l->bg = ugfx_rgb(245, 245, 245);
    l->rows = 1;
}

void uui_label_set_text(struct uui_label *l, const char *text) {
    l->text = text;
}

void uui_label_natural_size(const struct uui_label *l, int *out_w, int *out_h) {
    // The TEXT's width, but a height that does not depend on it: a
    // caption whose row count varied with its content would reflow the
    // page every time it changed. See CLAUDE.md on natural_size.
    if (out_w) *out_w = l->text ? ugfx_text_width(l->text) : 0;
    if (out_h) *out_h = ugfx_char_h() * (l->rows > 0 ? l->rows : 1);
}

void uui_label_draw(struct ugfx_surface *s, const struct uui_label *l) {
    if (!l->text || !l->text[0]) return;
    ugfx_draw_string_clipped(s, l->x, l->y, l->w, l->text, l->fg, l->bg);
}

static void label_draw_op(struct ugfx_surface *s, const void *w) {
    uui_label_draw(s, (const struct uui_label *)w);
}
static void label_natural_op(const void *w, int *out_w, int *out_h) {
    uui_label_natural_size((const struct uui_label *)w, out_w, out_h);
}
static void label_geometry_op(void *w, int x, int y, int width, int height) {
    struct uui_label *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
}

// NO `hit`, deliberately -- so the router never offers a label a press
// and a click goes to whatever is behind it. A caption that swallowed
// clicks would be a control that does nothing, which is the shape of
// bug this toolkit keeps a rule about.
const struct uui_widget_ops uui_label_ops = {
    .draw = label_draw_op,
    .natural_size = label_natural_op,
    .set_geometry = label_geometry_op,
};
