// See ui/uui_layout.h.
#include "ui/uui_layout.h"

// Font-derived defaults, so a laid-out window reflows with `fontsize`.
// A character cell of margin and half of one between items reproduces
// the spacing the hand-placed apps already used (Calculator's MARGIN 8
// / BTN_GAP 6 against an 8x17 cell) without any of them saying a number.
static int lay_margin(const struct uui_layout *l) {
    return l->margin > 0 ? l->margin : ugfx_char_w();
}
static int lay_gap(const struct uui_layout *l) {
    return l->gap > 0 ? l->gap : ugfx_char_w() / 2 + 2;
}

static void item_natural(const struct uui_item *it, int *w, int *h) {
    *w = 0; *h = 0;
    if (it->ops && it->ops->natural_size) it->ops->natural_size(it->widget, w, h);
}

static int grid_cols(const struct uui_layout *l) {
    return l->cols > 0 ? l->cols : 1;
}

static int grid_rows(const struct uui_layout *l) {
    int c = grid_cols(l);
    return (l->count + c - 1) / c;
}

// The largest natural size any child asks for. A grid's cells are
// uniform, so one oversized label widens every column rather than
// producing a ragged table -- which is what a button grid wants and
// what Calculator did by hand (BTN_W sized for "+/-", the widest).
static void grid_cell(const struct uui_layout *l, int *cw, int *ch) {
    *cw = 0; *ch = 0;
    for (int i = 0; i < l->count; i++) {
        int w, h;
        item_natural(&l->items[i], &w, &h);
        if (w > *cw) *cw = w;
        if (h > *ch) *ch = h;
    }
}

void uui_layout_natural_size(const struct uui_layout *l, int *out_w, int *out_h) {
    int m = lay_margin(l), g = lay_gap(l);
    int w = 0, h = 0;

    if (l->dir == UUI_GRID) {
        int cw, ch;
        grid_cell(l, &cw, &ch);
        int cols = grid_cols(l), rows = grid_rows(l);
        w = cols * cw + (cols > 1 ? (cols - 1) * g : 0);
        h = rows * ch + (rows > 1 ? (rows - 1) * g : 0);
    } else {
        for (int i = 0; i < l->count; i++) {
            int iw, ih;
            item_natural(&l->items[i], &iw, &ih);
            if (l->dir == UUI_COLUMN) {
                if (iw > w) w = iw;
                h += ih;
            } else {
                w += iw;
                if (ih > h) h = ih;
            }
        }
        if (l->count > 1) {
            if (l->dir == UUI_COLUMN) h += (l->count - 1) * g;
            else                      w += (l->count - 1) * g;
        }
    }

    if (out_w) *out_w = w + 2 * m;
    if (out_h) *out_h = h + 2 * m;
}

static void place(struct uui_item *it, int x, int y, int w, int h) {
    if (it->ops && it->ops->set_geometry) it->ops->set_geometry(it->widget, x, y, w, h);
}

void uui_layout_run(struct uui_layout *l, int x, int y, int w, int h) {
    l->x = x; l->y = y; l->w = w; l->h = h;

    int m = lay_margin(l), g = lay_gap(l);
    int ix = x + m, iy = y + m;
    int inner_w = w - 2 * m, inner_h = h - 2 * m;

    if (l->dir == UUI_GRID) {
        int cols = grid_cols(l), rows = grid_rows(l);
        // Divide the room available, not the natural size: a grid given
        // more than it asked for spreads into it rather than huddling in
        // the corner, and given less it shrinks rather than overflowing.
        int cw = cols > 0 ? (inner_w - (cols - 1) * g) / cols : inner_w;
        int ch = rows > 0 ? (inner_h - (rows - 1) * g) / rows : inner_h;
        for (int i = 0; i < l->count; i++) {
            int c = i % cols, r = i / cols;
            place(&l->items[i], ix + c * (cw + g), iy + r * (ch + g), cw, ch);
        }
        return;
    }

    // --- main-axis growth -------------------------------------------
    //
    // An item flagged to fill along the STACKING direction absorbs
    // whatever space the others did not use -- flexbox's flex-grow, in
    // its simplest form. Without this every child got its natural size
    // and a resized window simply grew empty space below the last one,
    // so nothing in a column could ever get taller: Task Manager's table
    // widened with its window and kept exactly the rows it opened with.
    //
    // The cross axis is handled per item below and is a different
    // question ("how wide is a row in this column"), which is why the
    // two flags do not mean the same thing in both directions:
    // UUI_FILL_H grows a COLUMN's children and stretches a ROW's.
    int grow_count = 0, natural_total = 0;
    for (int i = 0; i < l->count; i++) {
        int iw, ih;
        item_natural(&l->items[i], &iw, &ih);
        natural_total += (l->dir == UUI_COLUMN) ? ih : iw;

        unsigned grow_flag = (l->dir == UUI_COLUMN) ? UUI_FILL_H : UUI_FILL_W;
        if (l->items[i].flags & grow_flag) grow_count++;
    }
    if (l->count > 1) natural_total += (l->count - 1) * g;

    int inner_main = (l->dir == UUI_COLUMN) ? inner_h : inner_w;
    int spare = inner_main - natural_total;
    if (spare < 0) spare = 0; // given less than it wants, it overflows
                               // rather than shrinking children below
                               // their stated minimum -- the same call
                               // the grid above makes.
    int grow_each = grow_count > 0 ? spare / grow_count : 0;
    int grow_seen = 0;

    int cursor = (l->dir == UUI_COLUMN) ? iy : ix;
    for (int i = 0; i < l->count; i++) {
        struct uui_item *it = &l->items[i];
        int iw, ih;
        item_natural(it, &iw, &ih);

        // The LAST growing item takes the rounding remainder, so the
        // children exactly fill the container instead of leaving a
        // few pixels that read as a layout bug.
        unsigned grow_flag = (l->dir == UUI_COLUMN) ? UUI_FILL_H : UUI_FILL_W;
        if (it->flags & grow_flag) {
            grow_seen++;
            int extra = (grow_seen == grow_count)
                          ? spare - grow_each * (grow_count - 1) : grow_each;
            if (l->dir == UUI_COLUMN) ih += extra; else iw += extra;
        }

        // 0 means "no preference" (uui_primitives.h), so a child that
        // does not care gets the cross-axis size rather than nothing --
        // which is what makes a text field in a column come out the
        // width of the column instead of invisible.
        if (l->dir == UUI_COLUMN) {
            int cw = (iw <= 0 || (it->flags & UUI_FILL_W)) ? inner_w : iw;
            place(it, ix, cursor, cw, ih);
            cursor += ih + g;
        } else {
            int chh = (ih <= 0 || (it->flags & UUI_FILL_H)) ? inner_h : ih;
            place(it, cursor, iy, iw, chh);
            cursor += iw + g;
        }
    }
}

void uui_layout_draw(struct ugfx_surface *s, const struct uui_layout *l) {
    for (int i = 0; i < l->count; i++) {
        const struct uui_item *it = &l->items[i];
        if (it->ops && it->ops->draw) it->ops->draw(s, it->widget);
    }
}

// --- a layout as a widget, so containers nest -------------------------

static void layout_natural(const void *w, int *out_w, int *out_h) {
    uui_layout_natural_size((const struct uui_layout *)w, out_w, out_h);
}
static void layout_geometry(void *w, int x, int y, int width, int height) {
    uui_layout_run((struct uui_layout *)w, x, y, width, height);
}
static void layout_draw(struct ugfx_surface *s, const void *w) {
    uui_layout_draw(s, (const struct uui_layout *)w);
}

const struct uui_widget_ops uui_layout_ops = {
    .natural_size = layout_natural,
    .set_geometry = layout_geometry,
    .draw         = layout_draw,
};

// --- the custom item --------------------------------------------------

static void custom_natural(const void *w, int *out_w, int *out_h) {
    const struct uui_custom *c = (const struct uui_custom *)w;
    if (out_w) *out_w = c->w;
    if (out_h) *out_h = c->h;
}
static void custom_geometry(void *w, int x, int y, int width, int height) {
    struct uui_custom *c = (struct uui_custom *)w;
    c->x = x; c->y = y; c->w = width; c->h = height;
}
static void custom_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_custom *c = (const struct uui_custom *)w;
    if (c->draw) c->draw(s, c);
}

const struct uui_widget_ops uui_custom_ops = {
    .natural_size = custom_natural,
    .set_geometry = custom_geometry,
    .draw         = custom_draw,
};
