// See ui/uui_card.h.
#include "ui/uui_card.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"

static int pad(void)   { return ugfx_char_w() + 2; }
static int gap(void)   { return ugfx_char_h() / 2; }
static int line_h(void) { return ugfx_char_h() + 2; }

static int has_sub(const struct uui_card *c) { return c->subtitle && c->subtitle[0]; }
static int head_text_h(const struct uui_card *c) { return line_h() * (has_sub(c) ? 2 : 1); }

static void head_ctl_natural(const struct uui_card *c, int *w, int *h) {
    *w = *h = 0;
    const struct uui_item *k = &c->kids[0];
    if (k->ops && k->ops->natural_size) k->ops->natural_size(k->widget, w, h);
}

static int head_h(const struct uui_card *c) {
    int cw, ch;
    head_ctl_natural(c, &cw, &ch);
    int th = head_text_h(c);
    return th > ch ? th : ch;
}

void uui_card_init(struct uui_card *c, const char *title, const char *subtitle,
                   struct uui_item header, struct uui_item *items, int count) {
    *c = (struct uui_card){ .title = title, .subtitle = subtitle };
    c->kids[0] = header;
    c->body = (struct uui_layout){ .dir = UUI_COLUMN, .items = items, .count = count };
    c->kids[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->body, .flags = UUI_FILL_W };
}

static void natural(const void *w, int *ow, int *oh) {
    const struct uui_card *c = w;
    int bw = 0, bh = 0, cw, ch;
    uui_layout_natural_size(&c->body, &bw, &bh);
    head_ctl_natural(c, &cw, &ch);
    int hw = ugfx_text_width(c->title ? c->title : "") + gap() + cw;
    *ow = 2 * pad() + (hw > bw ? hw : bw);
    *oh = 2 * pad() + head_h(c) + (c->body.count ? gap() + bh : 0);
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_card *c = w;
    c->x = x; c->y = y; c->w = width; c->h = height;
    int hh = head_h(c);
    const struct uui_item *k = &c->kids[0];
    if (k->ops && k->ops->set_geometry) {
        int cw, ch;
        head_ctl_natural(c, &cw, &ch);
        k->ops->set_geometry(k->widget, x + width - pad() - cw, y + pad() + (hh - ch) / 2, cw, ch);
    }
    int by = y + pad() + hh + gap();
    uui_layout_ops.set_geometry(&c->body, x + pad(), by, width - 2 * pad(),
                                y + height - pad() - by);
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_card *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

static struct uui_item *children(void *w, int *n) {
    struct uui_card *c = w;
    if (c->kids[0].ops) { *n = 2; return c->kids; }
    *n = 1;
    return &c->kids[1];
}

static void paint(struct ugfx_surface *s, void *w) {
    const struct uui_card *c = w;
    uint32_t card = UTHEME_WHITE, fg = UTHEME_TEXT;
    uint32_t dim = uui_state_bg(fg, UUI_STATE_DISABLED);
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, 8, c->active ? UTHEME_ACCENT : UTHEME_SEPARATOR);
    uui_fill_round_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, 7, card);

    int cw, ch;
    head_ctl_natural(c, &cw, &ch);
    int tx = c->x + pad(), tw = c->w - 2 * pad() - (cw ? cw + gap() : 0);
    int ty = c->y + pad() + (head_h(c) - head_text_h(c)) / 2;
    if (tw <= 0) return;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    const char *title = c->title ? c->title : "";
    int title_w = ugfx_text_width(title);
    ugfx_draw_string_clipped(s, tx, ty, tw, title, fg, card);
    ugfx_set_font(was);
    if (c->badge && c->badge[0] && title_w + 3 * gap() < tw) {
        // A pill in the selection colour with accent text: a state, not a
        // control, so lighter than a filled button.
        int bx = tx + title_w + gap(), bw = ugfx_text_width(c->badge) + 2 * gap();
        if (bx + bw > tx + tw) bw = tx + tw - bx;
        uui_fill_round_rect(s, bx, ty, bw, ugfx_char_h() + 2, UUI_CAPSULE,
                            UTHEME_SELECTION);
        ugfx_draw_string_clipped(s, bx + gap(), ty + 1, bw - 2 * gap(), c->badge, UTHEME_ACCENT,
                                 UTHEME_SELECTION);
    }
    if (has_sub(c))
        ugfx_draw_string_clipped(s, tx, ty + line_h(), tw, c->subtitle, dim, card);
}

const struct uui_widget_ops uui_card_ops = {
    .natural_size   = natural,
    .set_geometry   = geometry,
    .bounds         = bounds,
    .children       = children,
    .children_begin = paint,
};
