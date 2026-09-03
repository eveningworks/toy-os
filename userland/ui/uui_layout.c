// See ui/uui_layout.h.
#include "ui/uui_layout.h"

// Font-derived defaults, so a laid-out window reflows with `fontsize`.
// A character cell of margin and half of one between items reproduces
// the spacing the hand-placed apps already used (Calculator's MARGIN 8
// / BTN_GAP 6 against an 8x17 cell) without any of them saying a number.
int uui_layout_margin(const struct uui_layout *l) {
    return l->margin > 0 ? l->margin : ugfx_char_w();
}
int uui_layout_gap(const struct uui_layout *l) {
    return l->gap > 0 ? l->gap : ugfx_char_w() / 2 + 2;
}

// The least a UUI_FILL child is ever squeezed to. See the shortfall
// note in uui_layout_run().
#define MIN_STRETCHED 8

// `dir` is the container's stacking axis, or -1 for a grid -- which
// ignores the pin, its cells being uniform by definition.
static void item_natural(const struct uui_item *it, int dir, int *w, int *h) {
    *w = 0; *h = 0;
    // A HIDDEN child asks for nothing, so it reserves no space. Without
    // this a hidden widget still pushed its siblings around -- a page
    // switch would leave a gap exactly the size of the page it hid,
    // which reads as a layout bug rather than as a hidden control.
    if (it->hidden) return;
    if (it->ops && it->ops->natural_size) it->ops->natural_size(it->widget, w, h);
    if (it->main_size > 0) {
        if (dir == UUI_COLUMN)   *h = it->main_size;
        else if (dir == UUI_ROW) *w = it->main_size;
    }
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
        item_natural(&l->items[i], -1, &w, &h);
        if (w > *cw) *cw = w;
        if (h > *ch) *ch = h;
    }
}

void uui_layout_natural_size(const struct uui_layout *l, int *out_w, int *out_h) {
    int m = uui_layout_margin(l), g = uui_layout_gap(l);
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
            item_natural(&l->items[i], l->dir, &iw, &ih);
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

    int m = uui_layout_margin(l), g = uui_layout_gap(l);
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
        item_natural(&l->items[i], l->dir, &iw, &ih);
        natural_total += (l->dir == UUI_COLUMN) ? ih : iw;

        unsigned grow_flag = (l->dir == UUI_COLUMN) ? UUI_FILL_H : UUI_FILL_W;
        // ...and it must not claim a share of the leftover space
        // either, or the visible children silently get less than they
        // should.
        if (!l->items[i].hidden && (l->items[i].flags & grow_flag)) grow_count++;
    }
    if (l->count > 1) natural_total += (l->count - 1) * g;

    int inner_main = (l->dir == UUI_COLUMN) ? inner_h : inner_w;
    int spare = inner_main - natural_total;

    // A SHORTFALL COMES OUT OF THE CHILDREN THAT SAID THEY STRETCH.
    //
    // `spare` is allowed to be negative here, so the same share-out
    // below that grows UUI_FILL_H children by an equal slice also
    // shrinks them by one. That is the honest reading of the flag: a
    // child that can absorb extra space can absorb a shortfall -- a
    // scroll view scrolls, a list shows fewer rows -- while a child
    // WITHOUT the flag stated a size it needs and keeps it.
    //
    // Before this, a container given less than it wanted handed every
    // child its full natural size and placed the remainder past its own
    // bottom edge. The children that fell off simply vanished, with
    // nothing on screen to say so: Control Panel shrunk below its
    // content lost its status bar entirely, and no amount of scrolling
    // within the page could bring back something laid out beyond the
    // window.
    //
    // With nothing stretchable to take it from, the old behaviour
    // stands -- overflowing is still better than squashing a child
    // below the size it said it needed.
    if (grow_count == 0 && spare < 0) spare = 0;
    int grow_each = grow_count > 0 ? spare / grow_count : 0;
    int grow_seen = 0;

    int cursor = (l->dir == UUI_COLUMN) ? iy : ix;
    for (int i = 0; i < l->count; i++) {
        struct uui_item *it = &l->items[i];
        int iw, ih;
        item_natural(it, l->dir, &iw, &ih);

        // The LAST growing item takes the rounding remainder, so the
        // children exactly fill the container instead of leaving a
        // few pixels that read as a layout bug.
        unsigned grow_flag = (l->dir == UUI_COLUMN) ? UUI_FILL_H : UUI_FILL_W;
        if (!it->hidden && (it->flags & grow_flag)) {
            grow_seen++;
            int extra = (grow_seen == grow_count)
                          ? spare - grow_each * (grow_count - 1) : grow_each;
            int size = ((l->dir == UUI_COLUMN) ? ih : iw) + extra;
            // A floor, because a stretchable child squeezed to nothing
            // is indistinguishable from a bug -- and leaving it a few
            // pixels keeps its scrollbar on screen to say there is more.
            if (size < MIN_STRETCHED) size = MIN_STRETCHED;
            if (l->dir == UUI_COLUMN) ih = size; else iw = size;
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
        // `hidden` means NOT DRAWN as well as not hit-tested, which is
        // what uui_widget.h has always promised and what this loop did
        // not do. uui_route.c's own draw honoured it, so an app
        // declaring both a layout and .widgets (the normal shape) got a
        // control that was unclickable and still perfectly visible --
        // the page it thought it had hidden painting over the page it
        // had switched to.
        if (it->hidden) continue;
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

// A layout is a placement device, not a widget with behaviour: it
// declares its items and nothing else, so the router recurses into them
// and each child reports its own id. It deliberately has no `hit` --
// that would CLIP its children to it, which is a scroll view's job and
// not a plain container's.
static struct uui_item *layout_children(void *w, int *out_count) {
    struct uui_layout *l = w;
    *out_count = l->count;
    return l->items;
}

static void layout_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_layout *s = (const struct uui_layout *)w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

const struct uui_widget_ops uui_layout_ops = {
    .children = layout_children,
    .natural_size = layout_natural,
    .set_geometry = layout_geometry,
    .draw         = layout_draw,
    .bounds = layout_bounds,
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

static void custom_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_custom *c = (const struct uui_custom *)w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

const struct uui_widget_ops uui_custom_ops = {
    .natural_size = custom_natural,
    .set_geometry = custom_geometry,
    .draw         = custom_draw,
    .bounds = custom_bounds,
};
