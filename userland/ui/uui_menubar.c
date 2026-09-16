// menu bar + nested pull-down menus. See ui/uui_menubar.h for the design.
#include "ui/uui_menubar.h"
#include "ui/uui_popup.h" // each level is a popup surface when one is granted
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include <string.h>
#include <ctype.h> // tolower() -- the toolkit's, over k_tolower

// ---------------------------------------------------------------------
// metrics -- all font-derived, per docs/gui-guidelines.md
// ---------------------------------------------------------------------

// **THE SPACING UNIT IS A REPRESENTATIVE GLYPH, NOT THE WIDEST ONE.**
// Every gap in this file used to be a multiple of ugfx_char_w(), which
// is the WIDEST advance in the face. On a monospace face that is the
// same number as every other advance, so it read as "one character".
// On a proportional one it is nearly twice the average, and the day the
// interface face stopped being monospace every title's padding doubled
// and the bar spread out.
//
// `advance('n')` is unchanged on a monospace face -- so this is a
// no-op for anything mono -- and is a sane average on a proportional
// one. Height would also have been stable (utheme_pad() derives from
// it), but it would have changed the monospace look too.
static int unit(void) {
    int u = ugfx_char_advance('n');
    return u > 0 ? u : ugfx_char_w();
}

static int pad(void)      { return unit() / 2; }
static int gutter(void)   { return unit() * 2; } // the tick column
static int arrow_col(void){ return unit(); }     // the submenu arrow
static int accel_gap(void){ return unit() * 2; }

static int row_height(void) { return ugfx_char_h() + 6; }

// A separator is a hairline with air above and below it, not a row.
static int sep_height(void) { int h = ugfx_char_h() / 3; return h < 5 ? 5 : h; }

static int is_sep(const struct uui_menu_item *it) { return it->label == 0; }
static int is_sub(const struct uui_menu_item *it) { return it->sub != 0 && it->sub_count > 0; }

static int item_height(const struct uui_menu_item *it) {
    return is_sep(it) ? sep_height() : row_height();
}

// The one place item state is asked for. A separator has no code and is
// never queried; a submenu IS queried, so an app can grey out a branch.
static unsigned flags_of(const struct uui_menubar *m, const struct uui_menu_item *it) {
    if (is_sep(it) || !m->item_flags) return 0;
    return m->item_flags(it->code);
}

static int enabled(const struct uui_menubar *m, const struct uui_menu_item *it) {
    return !is_sep(it) && !(flags_of(m, it) & UUI_MI_DISABLED);
}

static int title_w(const struct uui_menu_item *it) {
    return ugfx_text_width(it->label) + 2 * unit();
}

// ONE geometry for a popup, shared by placement, drawing and hit-testing
// -- the rule docs/gui-guidelines.md states for scrollbars and which
// applies just as hard here, where a click landing one row off is the
// classic symptom of two copies of this arithmetic.
static void level_size(const struct uui_menu_item *items, int count,
                        int *out_w, int *out_h) {
    int widest = 0, widest_accel = 0;
    int h = 2; // the 1px border, top and bottom

    for (int i = 0; i < count; i++) {
        const struct uui_menu_item *it = &items[i];
        h += item_height(it);
        if (is_sep(it)) continue;
        int lw = ugfx_text_width(it->label);
        if (lw > widest) widest = lw;
        if (!is_sub(it) && it->accel) {
            int aw = ugfx_text_width(it->accel);
            if (aw > widest_accel) widest_accel = aw;
        }
    }

    // The arrow column is reserved whether or not this menu has a
    // submenu, so labels line up between sibling menus.
    int w = 2 + pad() + gutter() + widest + arrow_col() + pad();
    if (widest_accel) w += accel_gap() + widest_accel;

    *out_w = w;
    *out_h = h;
}

// y offset of row `index` from the popup's top edge.
static int row_offset(const struct uui_menu_item *items, int index) {
    int y = 1;
    for (int i = 0; i < index; i++) y += item_height(&items[i]);
    return y;
}

// Which SELECTABLE row a point falls on, or -1. Separators deliberately
// answer -1: they are drawn but are not rows you can be on.
static int row_at(const struct uui_menu_level *lv, int dy) {
    int y = 1;
    for (int i = 0; i < lv->count; i++) {
        int h = item_height(&lv->items[i]);
        if (dy >= y && dy < y + h) return is_sep(&lv->items[i]) ? -1 : i;
        y += h;
    }
    return -1;
}

// ---------------------------------------------------------------------
// placement -- flip, then slide, then clamp
// ---------------------------------------------------------------------
//
// The same vocabulary as Wayland's xdg_positioner constraint adjustments
// (flip_x/flip_y/slide_x/slide_y), which is what KDE's menus resolve
// through and what Win32 does against the monitor work area. The only
// difference here is which rectangle it is resolved against -- see the
// header. `side` picks the preferred edge: 0 = below the anchor
// (a bar title), 1 = to its right (a submenu).

static void place(const struct uui_menubar *m, int ax, int ay, int aw, int ah,
                   int w, int h, int side, int *out_x, int *out_y) {
    int x, y;

    if (side == 0) {
        x = ax;
        y = ay + ah;
        if (y + h > m->by + m->bh && ay - h >= m->by) y = ay - h; // flip above
    } else {
        x = ax + aw;
        y = ay - 1; // the submenu's first row lines up with its parent row
        if (x + w > m->bx + m->bw && ax - w >= m->bx) x = ax - w; // flip left
    }

    if (x + w > m->bx + m->bw) x = m->bx + m->bw - w; // slide
    if (x < m->bx) x = m->bx;                          // clamp
    if (y + h > m->by + m->bh) y = m->by + m->bh - h;
    if (y < m->by) y = m->by;

    *out_x = x;
    *out_y = y;
}

// ---------------------------------------------------------------------
// opening and closing
// ---------------------------------------------------------------------

// EVERY PATH THAT SHORTENS THE CHAIN GOES THROUGH HERE, because a level
// may own a popup surface and dropping the count alone would leave the
// compositor showing a menu nothing hit-tests any more.
static void set_depth(struct uui_menubar *m, int n) {
    if (n < 0) n = 0;
    for (int l = n; l < m->depth; l++) {
        if (m->level[l].surf) uui_popup_close(m->level[l].surf);
        m->level[l].surf = 0;
    }
    m->depth = n;
}

void uui_menubar_close(struct uui_menubar *m) {
    m->open_root = -1;
    set_depth(m, 0);
}

// The compositor dismissed the chain -- a press outside every surface of
// this process (abi/win_proto.h's WIN_EV_POPUP_DONE). The ids are dead
// already; closing here is bookkeeping, and uui_popup_close() on a dead
// id is a no-op.
static void level_done(void *owner) {
    uui_menubar_close((struct uui_menubar *)owner);
}

// Opens level `l` on `items`, anchored to a rect: as a POPUP SURFACE
// when the provider grants one, drawn in-window against the bounds
// otherwise. `side` 0 = below the anchor (a title, the cursor), 1 = to
// its right (a submenu, whose first row lines up with its parent row --
// hence the anchor is nudged up by the popup's 1px border).
static void open_level(struct uui_menubar *m, int l,
                        const struct uui_menu_item *items, int count,
                        int ax, int ay, int aw, int ah, int side, int parent) {
    set_depth(m, l);

    int w, h;
    level_size(items, count, &w, &h);

    int px, py;
    // A MENU GRABS: a press outside must dismiss it and be swallowed,
    // which is the whole reason it is a surface and not a rectangle.
    int surf = uui_popup_open(ax, side ? ay - 1 : ay, aw, ah, w, h,
                              side ? UUI_POPUP_RIGHT : UUI_POPUP_BELOW,
                              UUI_POPUP_GRAB, level_done, m, &px, &py);
    if (!surf) place(m, ax, ay, aw, ah, w, h, side, &px, &py);

    struct uui_menu_level *lv = &m->level[l];
    lv->items = items;
    lv->count = count;
    lv->x = px; lv->y = py;
    lv->w = w;  lv->h = h;
    lv->hot = -1;
    lv->parent = parent;
    lv->surf = surf;
    m->depth = l + 1;
}

// Next selectable row from `from` in direction `dir`, wrapping. -1 if the
// menu has no selectable row at all (every item disabled).
static int step_sel(const struct uui_menubar *m, const struct uui_menu_item *items,
                     int count, int from, int dir) {
    if (count <= 0) return -1;
    int i = from;
    for (int n = 0; n < count; n++) {
        i = (i < 0) ? (dir > 0 ? 0 : count - 1) : (i + dir + count) % count;
        if (enabled(m, &items[i])) return i;
    }
    return -1;
}

static void open_root(struct uui_menubar *m, int index) {
    m->open_root = index;
    set_depth(m, 0);
    if (index < 0 || index >= m->count) return;

    const struct uui_menu_item *it = &m->items[index];
    if (!is_sub(it)) return; // a bare title commits nothing and opens nothing

    int tx, ty, tw, th;
    uui_menubar_title_rect(m, index, &tx, &ty, &tw, &th);
    open_level(m, 0, it->sub, it->sub_count, tx, ty, tw, th, 0, index);
}

// See the header: a context menu is level 0 with no title behind it, so
// `open_root` stays -1 and every path that walks the bar's titles finds
// nothing to walk.
void uui_menubar_open_at(struct uui_menubar *m, const struct uui_menu_item *items,
                          int count, int x, int y) {
    m->open_root = -1;
    set_depth(m, 0);
    if (!items || count <= 0) return;
    // The anchor is the cursor: one pixel wide and ZERO TALL, so
    // "below it" is (x, y) itself and the header's "top-left at (x, y)"
    // is what a caller gets. A 1px-tall anchor put it one row low --
    // caught by a WM menu that has to line up under a title-bar icon.
    open_level(m, 0, items, count, x, y, 1, 0, 0, -1);
}

// Opens level `lvl + 1` from row `index` of level `lvl`.
static void open_sub(struct uui_menubar *m, int lvl, int index) {
    if (lvl + 1 >= UUI_MENU_MAX_DEPTH) return;
    struct uui_menu_level *parent = &m->level[lvl];
    const struct uui_menu_item *it = &parent->items[index];
    if (!is_sub(it)) return;

    int ax, ay, aw, ah;
    uui_menubar_item_rect(m, lvl, index, &ax, &ay, &aw, &ah);
    open_level(m, lvl + 1, it->sub, it->sub_count, ax, ay, aw, ah, 1, index);
}

// ---------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------

void uui_menubar_init(struct uui_menubar *m, const struct uui_menu_item *items,
                       int count) {
    m->x = m->y = m->w = m->h = 0;
    m->items = items;
    m->count = count;
    m->open_root = -1;
    m->hot_root = -1;
    m->depth = 0;
    for (int l = 0; l < UUI_MENU_MAX_DEPTH; l++) m->level[l].surf = 0;
    m->bx = m->by = 0;
    m->bw = m->bh = 0;
    m->item_flags = 0;
    m->committed = -1;

    m->bar_bg      = ugfx_rgb(235, 235, 238);
    m->fg          = UUI_COLOR_UNSET;
    m->popup_bg    = ugfx_rgb(250, 250, 252);
    m->hot_bg      = UUI_COLOR_UNSET;
    m->border      = UUI_COLOR_UNSET;
    m->accel_fg    = ugfx_rgb(120, 125, 135);
    m->disabled_fg = ugfx_rgb(170, 172, 178);
}

// Resolved at DRAW time (utheme.h). Only the three roles the palette
// already names: `bar_bg`, `popup_bg`, `accel_fg` and `disabled_fg` are
// still literals, since no role matches them and mapping them to the
// nearest one would change what every menu looks like today.
static uint32_t m_fg(const struct uui_menubar *m)     { return UUI_COLOR(m->fg, UTHEME_TEXT); }
static uint32_t m_hot_bg(const struct uui_menubar *m) { return UUI_COLOR(m->hot_bg, UTHEME_SELECTION); }
static uint32_t m_border(const struct uui_menubar *m) { return UUI_COLOR(m->border, UTHEME_OUTLINE); }


void uui_menubar_set_geometry(struct uui_menubar *m, int x, int y, int w, int h) {
    m->x = x; m->y = y; m->w = w; m->h = h;
    if (m->bw <= 0 || m->bh <= 0) { m->bx = x; m->by = y; m->bw = w; m->bh = h; }
}

void uui_menubar_set_bounds(struct uui_menubar *m, int x, int y, int w, int h) {
    m->bx = x; m->by = y; m->bw = w; m->bh = h;
}

void uui_menubar_natural_size(const struct uui_menubar *m, int *out_w, int *out_h) {
    if (out_w) {
        int w = unit();
        for (int i = 0; i < m->count; i++) w += title_w(&m->items[i]);
        *out_w = w;
    }
    // +1 for the hairline that separates the strip from the content.
    if (out_h) *out_h = ugfx_char_h() + 7;
}

int uui_menubar_is_open(const struct uui_menubar *m) { return m->depth > 0; }
int uui_menubar_depth(const struct uui_menubar *m) { return m->depth; }

// ---------------------------------------------------------------------
// geometry queries
// ---------------------------------------------------------------------

int uui_menubar_title_rect(const struct uui_menubar *m, int index,
                            int *x, int *y, int *w, int *h) {
    if (index < 0 || index >= m->count) return 0;
    int tx = m->x + unit() / 2;
    for (int i = 0; i < index; i++) tx += title_w(&m->items[i]);
    if (x) *x = tx;
    if (y) *y = m->y;
    if (w) *w = title_w(&m->items[index]);
    if (h) *h = m->h - 1; // above the hairline
    return 1;
}

int uui_menubar_popup_rect(const struct uui_menubar *m, int level,
                            int *x, int *y, int *w, int *h) {
    if (level < 0 || level >= m->depth) return 0;
    const struct uui_menu_level *lv = &m->level[level];
    if (x) *x = lv->x;
    if (y) *y = lv->y;
    if (w) *w = lv->w;
    if (h) *h = lv->h;
    return 1;
}

int uui_menubar_item_rect(const struct uui_menubar *m, int level, int index,
                           int *x, int *y, int *w, int *h) {
    if (level < 0 || level >= m->depth) return 0;
    const struct uui_menu_level *lv = &m->level[level];
    if (index < 0 || index >= lv->count) return 0;
    if (x) *x = lv->x;
    if (y) *y = lv->y + row_offset(lv->items, index);
    if (w) *w = lv->w;
    if (h) *h = item_height(&lv->items[index]);
    return 1;
}

static int title_at(const struct uui_menubar *m, int cx, int cy) {
    for (int i = 0; i < m->count; i++) {
        int x, y, w, h;
        uui_menubar_title_rect(m, i, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, cx, cy)) return i;
    }
    return -1;
}

int uui_menubar_hit(const struct uui_menubar *m, int cx, int cy) {
    if (uui_hit(m->x, m->y, m->w, m->h, cx, cy)) return 1;
    for (int l = 0; l < m->depth; l++) {
        const struct uui_menu_level *lv = &m->level[l];
        if (uui_hit(lv->x, lv->y, lv->w, lv->h, cx, cy)) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------

static void draw_tick(struct ugfx_surface *s, int x, int y, uint32_t fg) {
    int ch = ugfx_char_h();
    int mid = ch / 2;
    ugfx_draw_line(s, x + 1, y + mid, x + ch / 3, y + ch - 3, fg, GEOM_AA);
    ugfx_draw_line(s, x + ch / 3, y + ch - 3, x + ch - 2, y + 2, fg, GEOM_AA);
}

static void draw_arrow(struct ugfx_surface *s, int cx, int cy, uint32_t fg) {
    int r = ugfx_char_h() / 4;
    if (r < 3) r = 3;
    for (int i = 0; i <= r; i++)
        ugfx_fill_rect(s, cx - r / 2 + i, cy - (r - i), 1, 2 * (r - i) + 1, fg);
}

void uui_menubar_draw(struct ugfx_surface *s, const struct uui_menubar *m) {
    ugfx_fill_rect(s, m->x, m->y, m->w, m->h, m->bar_bg);
    ugfx_fill_rect(s, m->x, m->y + m->h - 1, m->w, 1, m_border(m));

    for (int i = 0; i < m->count; i++) {
        int x, y, w, h;
        uui_menubar_title_rect(m, i, &x, &y, &w, &h);

        // An OPEN title reads as pressed, because it is: the button is
        // conceptually still down for as long as its menu is showing.
        enum uui_state st = UUI_STATE_REST;
        if (m->depth > 0 && m->open_root == i) st = UUI_STATE_PRESSED;
        else if (m->hot_root == i) st = UUI_STATE_HOVER;

        uint32_t bg = uui_state_bg(m->bar_bg, st);
        if (st != UUI_STATE_REST) ugfx_fill_rect(s, x, y, w, h, bg);
        ugfx_draw_string_clipped(s, x + unit(), y + (h - ugfx_char_h()) / 2,
                                  w - 2 * unit() + 2, m->items[i].label,
                                  m_fg(m), bg);
    }
}

// One level, with its origin moved by (-ox, -oy): 0,0 when it is drawn
// into the window it hit-tests in, the level's own x/y when it is drawn
// into its popup surface, whose top-left IS the level's.
static void draw_level_at(struct ugfx_surface *s, const struct uui_menubar *m,
                           const struct uui_menu_level *lv, int ox, int oy) {
    int lx = lv->x - ox, ly = lv->y - oy;
    ugfx_fill_rect(s, lx, ly, lv->w, lv->h, m->popup_bg);
    ugfx_draw_rect(s, lx, ly, lv->w, lv->h, m_border(m));

    int y = ly + 1;
    for (int i = 0; i < lv->count; i++) {
        const struct uui_menu_item *it = &lv->items[i];
        int h = item_height(it);

        if (is_sep(it)) {
            ugfx_fill_rect(s, lx + pad(), y + h / 2, lv->w - 2 * pad(), 1, m_border(m));
            y += h;
            continue;
        }

        unsigned f = flags_of(m, it);
        int off = (f & UUI_MI_DISABLED) != 0;
        uint32_t bg = m->popup_bg;
        if (i == lv->hot && !off) {
            bg = m_hot_bg(m);
            ugfx_fill_rect(s, lx + 1, y, lv->w - 2, h, bg);
        }
        uint32_t fg = off ? m->disabled_fg : m_fg(m);
        int ty = y + (h - ugfx_char_h()) / 2;

        if (f & UUI_MI_CHECKED) draw_tick(s, lx + 1 + pad(), ty, fg);

        int label_x = lx + 1 + pad() + gutter();
        int right = lx + lv->w - 1 - pad() - arrow_col();
        int avail = right - label_x;

        if (is_sub(it)) {
            draw_arrow(s, right + arrow_col() / 2, y + h / 2, fg);
        } else if (it->accel) {
            int aw = ugfx_text_width(it->accel);
            avail -= aw + accel_gap();
            ugfx_draw_string_clipped(s, right - aw, ty, aw, it->accel,
                                      off ? m->disabled_fg : m->accel_fg, bg);
        }

        ugfx_draw_string_clipped(s, label_x, ty, avail, it->label, fg, bg);
        y += h;
    }
}

static void draw_level(struct ugfx_surface *s, const struct uui_menubar *m,
                        const struct uui_menu_level *lv) {
    if (lv->surf) {
        struct ugfx_surface *ps = uui_popup_surface(lv->surf);
        if (ps) { draw_level_at(ps, m, lv, lv->x, lv->y); return; }
    }
    draw_level_at(s, m, lv, 0, 0);
}

void uui_menubar_draw_popup(struct ugfx_surface *s, const struct uui_menubar *m) {
    // Outermost first: a child overlaps its parent's right edge, so call
    // order IS z-order here too (and the compositor stacks the surfaces
    // in the order they were opened, which is the same order).
    for (int l = 0; l < m->depth; l++) draw_level(s, m, &m->level[l]);
}

// ---------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------

int uui_menubar_press(struct uui_menubar *m, int cx, int cy) {
    int t = title_at(m, cx, cy);
    if (t >= 0) {
        // Clicking the open title closes it, as every real menu bar does.
        if (m->depth > 0 && m->open_root == t) uui_menubar_close(m);
        else open_root(m, t);
        return 1;
    }

    if (uui_hit(m->x, m->y, m->w, m->h, cx, cy)) {
        uui_menubar_close(m); // the bar's empty space is still the bar's
        return 1;
    }

    if (m->depth > 0) {
        for (int l = 0; l < m->depth; l++) {
            const struct uui_menu_level *lv = &m->level[l];
            if (uui_hit(lv->x, lv->y, lv->w, lv->h, cx, cy)) {
                uui_menubar_motion(m, cx, cy);
                return 1;
            }
        }
        // A click anywhere else DISMISSES rather than falling through to
        // whatever is underneath -- an open menu owns the next click.
        uui_menubar_close(m);
        return 1;
    }
    return 0;
}

int uui_menubar_motion(struct uui_menubar *m, int cx, int cy) {
    int changed = 0;

    int t = title_at(m, cx, cy);
    if (t != m->hot_root) { m->hot_root = t; changed = 1; }
    if (m->depth <= 0) return changed;

    // Sliding along the bar with a menu open switches menus -- Windows,
    // KDE, GTK and macOS all do this, and it is what makes a menu bar
    // browsable in one gesture.
    if (t >= 0) {
        if (t != m->open_root) { open_root(m, t); return 1; }
        return changed;
    }

    for (int l = m->depth - 1; l >= 0; l--) {
        struct uui_menu_level *lv = &m->level[l];
        if (!uui_hit(lv->x, lv->y, lv->w, lv->h, cx, cy)) continue;

        int idx = row_at(lv, cy - lv->y);
        if (idx != lv->hot) { lv->hot = idx; changed = 1; }

        int want = l + 1;
        if (idx >= 0 && is_sub(&lv->items[idx]) && enabled(m, &lv->items[idx])) {
            // Re-opening an already-open submenu every motion event would
            // repaint continuously, which is the on_hover contract's
            // "return 1 only when it actually changed" in another guise.
            if (m->depth <= l + 1 || m->level[l + 1].parent != idx) {
                open_sub(m, l, idx);   // closes anything deeper first
                changed = 1;
            }
            want = m->depth;
        }
        if (m->depth != want) { set_depth(m, want); changed = 1; }
        return changed;
    }

    // Off every popup: the chain stays as it is. Sweeping the cursor
    // diagonally toward a submenu leaves the parent briefly, and closing
    // there would make submenus nearly unreachable.
    return changed;
}

int uui_menubar_release(struct uui_menubar *m, int cx, int cy) {
    if (m->depth <= 0) return -1;

    for (int l = m->depth - 1; l >= 0; l--) {
        const struct uui_menu_level *lv = &m->level[l];
        if (!uui_hit(lv->x, lv->y, lv->w, lv->h, cx, cy)) continue;

        int idx = row_at(lv, cy - lv->y);
        if (idx < 0) return -1;
        const struct uui_menu_item *it = &lv->items[idx];
        if (!enabled(m, it) || is_sub(it)) return -1;

        int code = it->code;
        uui_menubar_close(m);
        return code;
    }
    return -1;
}

// ---------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------

static void hot_to_first(struct uui_menubar *m) {
    if (m->depth <= 0) return;
    struct uui_menu_level *lv = &m->level[m->depth - 1];
    lv->hot = step_sel(m, lv->items, lv->count, -1, +1);
}

static void move_root(struct uui_menubar *m, int dir) {
    if (m->count <= 0) return;
    int i = m->open_root;
    if (i < 0) i = 0;
    open_root(m, (i + dir + m->count) % m->count);
    hot_to_first(m);
}

// Enter/Space, and the unique-letter case. 1 if consumed.
static int activate(struct uui_menubar *m, int *out_code) {
    struct uui_menu_level *lv = &m->level[m->depth - 1];
    if (lv->hot < 0) return 1;
    const struct uui_menu_item *it = &lv->items[lv->hot];
    if (!enabled(m, it)) return 1;

    if (is_sub(it)) {
        open_sub(m, m->depth - 1, lv->hot);
        hot_to_first(m);
        return 1;
    }
    if (out_code) *out_code = it->code;
    uui_menubar_close(m);
    return 1;
}

// k_tolower() is the toolkit's (reached as tolower() here, lib/string.h)
// -- this was a private copy of a function that already existed.
#define lower(c) ((char)tolower((int)(unsigned char)(c)))

static int letter_jump(struct uui_menubar *m, int key, int *out_code) {
    struct uui_menu_level *lv = &m->level[m->depth - 1];
    char want = lower((char)key);

    int matches = 0, first = -1;
    for (int i = 0; i < lv->count; i++) {
        if (!enabled(m, &lv->items[i])) continue;
        if (lower(lv->items[i].label[0]) != want) continue;
        matches++;
        if (first < 0) first = i;
    }
    if (matches == 0) return 1; // an open menu swallows the key regardless

    if (matches == 1) {
        lv->hot = first;
        return activate(m, out_code); // unique match acts, as in Windows
    }

    // Several: cycle to the next one after the highlight.
    for (int n = 1; n <= lv->count; n++) {
        int i = (lv->hot + n + lv->count) % lv->count;
        if (lv->hot < 0) i = (n - 1) % lv->count;
        if (!enabled(m, &lv->items[i])) continue;
        if (lower(lv->items[i].label[0]) == want) { lv->hot = i; return 1; }
    }
    return 1;
}

int uui_menubar_key(struct uui_menubar *m, int key, int *out_code) {
    if (m->depth <= 0) {
        if (key == KEY_F10 && m->count > 0) {
            open_root(m, 0);
            hot_to_first(m);
            return 1;
        }
        return 0;
    }

    switch (key) {
    case 0x1B: // Esc closes the deepest menu, not the whole chain
        if (m->depth > 1) set_depth(m, m->depth - 1);
        else uui_menubar_close(m);
        return 1;

    case KEY_ARROW_UP:
    case KEY_ARROW_DOWN: {
        struct uui_menu_level *lv = &m->level[m->depth - 1];
        lv->hot = step_sel(m, lv->items, lv->count, lv->hot,
                            key == KEY_ARROW_DOWN ? +1 : -1);
        return 1;
    }

    case KEY_ARROW_RIGHT: {
        struct uui_menu_level *lv = &m->level[m->depth - 1];
        if (lv->hot >= 0 && is_sub(&lv->items[lv->hot]) && enabled(m, &lv->items[lv->hot])) {
            open_sub(m, m->depth - 1, lv->hot);
            hot_to_first(m);
            return 1;
        }
        move_root(m, +1);
        return 1;
    }

    case KEY_ARROW_LEFT:
        if (m->depth > 1) { set_depth(m, m->depth - 1); return 1; }
        move_root(m, -1);
        return 1;

    case '\n':
    case '\r':
    case ' ':
        return activate(m, out_code);

    default:
        break;
    }

    if (key >= 33 && key < 127) return letter_jump(m, key, out_code);
    return 1; // an open menu owns the keyboard
}

// --- as a routed widget -------------------------------------------------
//
// See ui/uui_menubar.h. The whole point of this table is `overlay_active`:
// the popup is drawn outside the bar's own rect and over whatever is
// below it, so the router has to offer this widget every press BEFORE it
// hit-tests anything else.

int uui_menubar_take_code(struct uui_menubar *m) {
    int c = m->committed;
    m->committed = -1;
    return c;
}

static void ops_natural(const void *w, int *ow, int *oh) {
    uui_menubar_natural_size((const struct uui_menubar *)w, ow, oh);
}

static void ops_geometry(void *w, int x, int y, int width, int height) {
    uui_menubar_set_geometry((struct uui_menubar *)w, x, y, width, height);
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_menubar *m = (const struct uui_menubar *)w;
    if (x)  *x  = m->x;
    if (y)  *y  = m->y;
    if (ow) *ow = m->w;
    if (oh) *oh = m->h;
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    uui_menubar_draw(s, (const struct uui_menubar *)w);
}

static void ops_draw_overlay(struct ugfx_surface *s, const void *w) {
    uui_menubar_draw_popup(s, (const struct uui_menubar *)w);
}

static int ops_overlay_active(const void *w) {
    return uui_menubar_is_open((const struct uui_menubar *)w);
}

static int ops_hit(const void *w, int cx, int cy) {
    return uui_menubar_hit((const struct uui_menubar *)w, cx, cy);
}

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    return uui_menubar_press((struct uui_menubar *)w, cx, cy);
}

static int ops_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    return uui_menubar_motion((struct uui_menubar *)w, cx, cy);
}

static int ops_release(void *w, int cx, int cy) {
    struct uui_menubar *m = (struct uui_menubar *)w;
    int code = uui_menubar_release(m, cx, cy);
    if (code < 0) return 0;
    m->committed = code;
    return 1;
}

// title i, popup l, item l i -- the rects tests click, in one vocabulary
// for every app (ui/uui_describe.h). `open` says whether a popup is up.
static void ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_menubar *m = (const struct uui_menubar *)w;
    int x, y, wd, h;
    for (int i = 0; i < m->count; i++)
        if (uui_menubar_title_rect(m, i, &x, &y, &wd, &h))
            uui_describe_rect_i(d, "title", i, x, y, wd, h);
    for (int l = 0; l < m->depth; l++) {
        if (uui_menubar_popup_rect(m, l, &x, &y, &wd, &h))
            uui_describe_rect_i(d, "popup", l, x, y, wd, h);
        for (int i = 0; uui_menubar_item_rect(m, l, i, &x, &y, &wd, &h); i++)
            uui_describe_rect_ij(d, "item", l, i, x, y, wd, h);
    }
    uui_describe_int(d, "open", m->depth > 0);
}

const struct uui_widget_ops uui_menubar_ops = {
    .natural_size   = ops_natural,
    .set_geometry   = ops_geometry,
    .bounds         = ops_bounds,
    .draw           = ops_draw,
    .draw_overlay   = ops_draw_overlay,
    .hit            = ops_hit,
    .press          = ops_press,
    .motion         = ops_motion,
    .release        = ops_release,
    .overlay_active = ops_overlay_active,
    .describe       = ops_describe,
};
