// See ui/uui_setting_row.h.
#include "ui/uui_setting_row.h"
#include "ui/uui_primitives.h"
#include "ui/uui_label.h"   // uui_label_wrap_next()
#include "ui/utheme.h"

#define ROW_DESC_MAX 4

static int pad_x(void) { return ugfx_char_w(); }
static int pad_y(void) { return ugfx_char_h() / 2 + 2; }
static int gap(void)   { return ugfx_char_w(); }
static int line_h(void) { return ugfx_char_h() + 2; }

static int is_stacked(const struct uui_setting_row *r) {
    return r->stacked || r->stacked_auto;
}

static void control_natural(const struct uui_setting_row *r, int *w, int *h) {
    *w = 0; *h = 0;
    if (r->control.ops && r->control.ops->natural_size)
        r->control.ops->natural_size(r->control.widget, w, h);
}

// The width the control gets beside the text. A FILL_W control (a field,
// a slider) has no width of its own worth keeping, so it gets two fifths.
static int control_w(const struct uui_setting_row *r, int avail) {
    int cw, ch;
    control_natural(r, &cw, &ch);
    if (r->control.flags & UUI_FILL_W) {
        int f = avail * 2 / 5;
        if (cw < f) cw = f;
    }
    return cw;
}

static int text_w(const struct uui_setting_row *r) {
    int inner = r->w - 2 * pad_x();
    if (is_stacked(r)) return inner;
    return inner - control_w(r, inner) - gap();
}

int uui_setting_row_text_w(const struct uui_setting_row *r) { return text_w(r); }

static int has_desc(const struct uui_setting_row *r) { return r->desc && r->desc[0]; }

void uui_setting_row_init(struct uui_setting_row *r, const char *title,
                          const char *desc, struct uui_item control) {
    *r = (struct uui_setting_row){ .title = title, .desc = desc, .control = control,
                                   .desc_rows = 1 };
}

int uui_setting_row_fit(struct uui_setting_row *r) {
    if (r->w <= 0) return 0;
    int inner = r->w - 2 * pad_x();
    int was_auto = r->stacked_auto, was_rows = r->desc_rows;
    // Beside the text only while the text keeps at least half the card.
    r->stacked_auto = control_w(r, inner) + gap() > inner / 2;
    int rows = 1;
    if (has_desc(r)) {
        int tw = text_w(r);
        char line[160];
        const char *p = r->desc;
        rows = 0;
        while (*p && rows < ROW_DESC_MAX && tw > 0) {
            p = uui_label_wrap_next(p, tw, line, (int)sizeof line);
            rows++;
        }
        if (rows < 1) rows = 1;
    }
    r->desc_rows = rows;
    return r->stacked_auto != was_auto || r->desc_rows != was_rows;
}

static int text_h(const struct uui_setting_row *r) {
    return line_h() + (has_desc(r) ? r->desc_rows * line_h() : 0);
}

static void natural(const void *w, int *ow, int *oh) {
    const struct uui_setting_row *r = w;
    int cw, ch;
    control_natural(r, &cw, &ch);
    int th = text_h(r);
    *ow = 2 * pad_x() + cw;       // the text wraps, so it asks no width
    *oh = 2 * pad_y() + (is_stacked(r) ? th + pad_y() + ch : (th > ch ? th : ch));
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_setting_row *r = w;
    r->x = x; r->y = y; r->w = width; r->h = height;
    const struct uui_item *c = &r->control;
    if (!c->ops || !c->ops->set_geometry) return;
    int cw, ch;
    control_natural(r, &cw, &ch);
    int inner = width - 2 * pad_x();
    if (is_stacked(r)) {
        int ww = (c->flags & UUI_FILL_W) ? inner : (cw < inner ? cw : inner);
        c->ops->set_geometry(c->widget, x + pad_x(), y + pad_y() + text_h(r) + pad_y(), ww, ch);
    } else {
        int ww = control_w(r, inner);
        c->ops->set_geometry(c->widget, x + width - pad_x() - ww,
                             y + (height - ch) / 2, ww, ch);
    }
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_setting_row *r = w;
    *x = r->x; *y = r->y; *ow = r->w; *oh = r->h;
}

static struct uui_item *children(void *w, int *n) {
    struct uui_setting_row *r = w;
    *n = r->control.ops ? 1 : 0;
    return &r->control;
}

// The card, then its words -- under the control, which the router paints
// next. Beside the control the text block is centred on the card's height.
static void paint(struct ugfx_surface *s, void *w) {
    const struct uui_setting_row *r = w;
    uint32_t card = UTHEME_WHITE, fg = UTHEME_TEXT;
    uint32_t dim = uui_state_bg(fg, UUI_STATE_DISABLED);
    if (!r->flat) {
        uui_fill_round_rect(s, r->x, r->y, r->w, r->h, 6, UTHEME_SEPARATOR);
        uui_fill_round_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 5, card);
    }

    int tx = r->x + pad_x(), tw = text_w(r);
    int ty = is_stacked(r) ? r->y + pad_y() : r->y + (r->h - text_h(r)) / 2;
    if (tw <= 0) return;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int title_w = ugfx_text_width(r->title ? r->title : "");
    ugfx_draw_string_clipped(s, tx, ty, tw, r->title ? r->title : "",
                             r->disabled ? dim : fg, card);
    ugfx_set_font(was);
    // EDITED, NOT APPLIED: an accent dot after the name.
    if (r->changed) {
        int d = ugfx_char_h() / 3 + 2;
        int dx = tx + (title_w < tw - d - 4 ? title_w : tw - d - 4) + 6;
        uui_fill_round_rect(s, dx, ty + (ugfx_char_h() - d) / 2, d, d, UUI_CAPSULE,
                            UTHEME_ACCENT);
    }
    if (!has_desc(r)) return;
    char line[160];
    const char *p = r->desc;
    for (int i = 0; i < r->desc_rows && *p; i++) {
        p = uui_label_wrap_next(p, tw, line, (int)sizeof line);
        ugfx_draw_string_clipped(s, tx, ty + line_h() * (i + 1), tw, line, dim, card);
    }
}

const struct uui_widget_ops uui_setting_row_ops = {
    .natural_size   = natural,
    .set_geometry   = geometry,
    .bounds         = bounds,
    .children       = children,
    .children_begin = paint,
};
