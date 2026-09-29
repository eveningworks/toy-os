// toolbar -- see ui/uui_toolbar.h for the contract (one item_flags
// callback shared with the menu bar, tooltips on the app's tick).
#include "ui/uui_toolbar.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "ui/uui_popup.h"
#include "lib/icon_cache.h"
#include "rt/sys.h" // sys_ticks()
#include <string.h>

// Font-derived, per docs/gui-guidelines.md.
// A COMPACT strip (a status bar's view switch) draws its icons at the
// text's own height; the ordinary one a little larger.
static int icon_px(const struct uui_toolbar *t) { return ugfx_char_h() + (t->compact ? -2 : 4); }
static int btn_w(const struct uui_toolbar *t)   { return icon_px(t) + (t->compact ? 8 : 10); }
static int btn_h(const struct uui_toolbar *t)   { return icon_px(t) + (t->compact ? 2 : 8); }
static int sep_w(void)    { return 7; }
static int chev_w(void)   { return 10; }

static int is_sep_item(const struct uui_toolbar_item *it) { return it->icon == 0 && !it->label; }

// A button's width: icon-only is square-ish; a label and a chevron add
// their own widths, so the strip reflows with the font.
static int item_w(const struct uui_toolbar *t, const struct uui_toolbar_item *it) {
    if (is_sep_item(it)) return sep_w();
    if (!it->label && !(it->flags & UUI_TB_MENU)) return btn_w(t);
    int w = 12;
    if (it->icon) w += icon_px(t);
    if (it->label) w += (it->icon ? 6 : 0) + ugfx_text_width(it->label);
    if (it->flags & UUI_TB_MENU) w += chev_w();
    return w < btn_w(t) ? btn_w(t) : w;
}

void uui_toolbar_init(struct uui_toolbar *t,
                       const struct uui_toolbar_item *items, int count) {
    memset(t, 0, sizeof *t);
    t->items = items;
    t->count = count;
    t->hot = -1;
    t->armed = -1;
    t->committed = -1;
    t->bg = ugfx_rgb(238, 238, 242);
    t->fg = ugfx_rgb(20, 20, 20);
    t->border = ugfx_rgb(200, 200, 208);
    t->tip_bg = ugfx_rgb(255, 252, 220); // the tooltip cream every desktop uses
    t->tip_fg = ugfx_rgb(20, 20, 20);
}

static int is_sep(const struct uui_toolbar_item *it) { return is_sep_item(it); }

static unsigned flags_of(const struct uui_toolbar *t, int i) {
    if (i < 0 || i >= t->count || is_sep(&t->items[i])) return 0;
    return t->item_flags ? t->item_flags(t->items[i].code) : 0;
}

void uui_toolbar_natural_size(const struct uui_toolbar *t, int *out_w, int *out_h) {
    int w = 4;
    for (int i = 0; i < t->count; i++) w += item_w(t, &t->items[i]);
    if (out_w) *out_w = w + 4;
    if (out_h) *out_h = btn_h(t) + (t->compact ? 0 : 4);
}

// The first item of the right-hand group, or `count` when there is none.
static int end_group(const struct uui_toolbar *t) {
    for (int i = 0; i < t->count; i++)
        if (t->items[i].flags & UUI_TB_END) return i;
    return t->count;
}

int uui_toolbar_item_rect(const struct uui_toolbar *t, int i,
                           int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= t->count) return 0;
    int e = end_group(t);
    int ix = t->x + 4, from = 0;
    if (i >= e) {
        // Packed against the right edge, never over the left group.
        int ew = 0, lw = 4;
        for (int j = e; j < t->count; j++) ew += item_w(t, &t->items[j]);
        for (int j = 0; j < e; j++) lw += item_w(t, &t->items[j]);
        ix = t->x + t->w - 4 - ew;
        if (ix < t->x + lw) ix = t->x + lw;
        from = e;
    }
    for (int j = from; j < i; j++) ix += item_w(t, &t->items[j]);
    if (x) *x = ix;
    if (y) *y = t->y + (t->compact ? 0 : 2);
    if (w) *w = item_w(t, &t->items[i]);
    if (h) *h = btn_h(t);
    return 1;
}

int uui_toolbar_hit_item(const struct uui_toolbar *t, int cx, int cy) {
    for (int i = 0; i < t->count; i++) {
        int x, y, w, h;
        uui_toolbar_item_rect(t, i, &x, &y, &w, &h);
        if (!uui_hit(x, y, w, h, cx, cy)) continue;
        if (is_sep(&t->items[i])) return -1;
        if (flags_of(t, i) & UUI_MI_DISABLED) return -1;
        return i;
    }
    return -1;
}

int uui_toolbar_take_code(struct uui_toolbar *t) {
    int code = t->committed;
    t->committed = -1;
    return code;
}

// The tip's size, and the button it hangs off. Both the open site and
// the in-window draw need these and must agree, or the surface is sized
// for one tip and painted with another.
static int tip_metrics(const struct uui_toolbar *t, int *w, int *h,
                       int *bx, int *by, int *bw, int *bh) {
    if (t->hot < 0 || !t->items[t->hot].tip) return 0;
    uui_toolbar_item_rect(t, t->hot, bx, by, bw, bh);
    *w = ugfx_text_width(t->items[t->hot].tip) + UUI_TIP_PAD * 2;
    *h = ugfx_char_h() + UUI_TIP_PAD * 2;
    return 1;
}

static void tip_hide(struct uui_toolbar *t) {
    if (t->tip_popup) {
        uui_popup_close(t->tip_popup);
        t->tip_popup = 0;
    }
    t->tip_shown = 0;
}

// The compositor swept this client's popups -- it does that for ALL of
// them when a GRABBING one is dismissed, so a tooltip up beside an open
// menu goes with it. The id is already dead; just forget it.
static void tip_done(void *owner) {
    struct uui_toolbar *t = (struct uui_toolbar *)owner;
    t->tip_popup = 0;
    t->tip_shown = 0;
}

int uui_toolbar_tick(struct uui_toolbar *t) {
    int want = t->hot >= 0 && t->items[t->hot].tip &&
                sys_ticks() - t->hot_since >= UUI_TOOLTIP_DELAY_TICKS;
    if (want == t->tip_shown) return 0;
    if (!want) { tip_hide(t); return 1; }

    t->tip_shown = 1;
    int w, h, bx, by, bw, bh;
    if (tip_metrics(t, &w, &h, &bx, &by, &bw, &bh)) {
        int px, py;
        // NO GRAB. The compositor flips it above the button when there
        // is no room below, which is the clamp this widget used to do
        // against the surface -- and could only ever do against its
        // OWN window.
        t->tip_popup = uui_popup_open(bx, by, bw, bh, w, h, UUI_POPUP_BELOW,
                                      0, tip_done, t, &px, &py);
    }
    return 1;
}

static void tb_draw(struct ugfx_surface *s, const struct uui_toolbar *t) {
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, t->bg);
    if (!t->compact) ugfx_fill_rect(s, t->x, t->y + t->h - 1, t->w, 1, t->border);

    for (int i = 0; i < t->count; i++) {
        int x, y, w, h;
        uui_toolbar_item_rect(t, i, &x, &y, &w, &h);
        if (is_sep(&t->items[i])) {
            ugfx_fill_rect(s, x + sep_w() / 2, y + 3, 1, h - 6, t->border);
            continue;
        }
        unsigned f = flags_of(t, i);

        // A LATCHED toggle draws pressed-in; pressing wins over hover,
        // uui_button's own ordering. All washes come from uui_state_bg
        // so hover darkens on this theme like everything else.
        enum uui_state st = UUI_STATE_REST;
        if (f & UUI_MI_DISABLED)                   st = UUI_STATE_DISABLED;
        else if (i == t->armed || (f & UUI_MI_CHECKED)) st = UUI_STATE_PRESSED;
        else if (i == t->hot)                      st = UUI_STATE_HOVER;
        if (st != UUI_STATE_REST)
            ugfx_fill_rect(s, x, y, w, h, uui_state_bg(t->bg, st));
        if (f & UUI_MI_CHECKED)
            ugfx_draw_rect(s, x, y, w, h, t->border);

        const struct uui_toolbar_item *it = &t->items[i];
        if (it->label || (it->flags & UUI_TB_MENU)) {
            // Icon, word, chevron, left to right.
            uint32_t bg = st != UUI_STATE_REST ? uui_state_bg(t->bg, st) : t->bg;
            uint32_t fg = (f & UUI_MI_DISABLED) ? uui_state_bg(t->fg, UUI_STATE_DISABLED) : t->fg;
            int cx = x + 6;
            const struct uimg *li = it->icon ? icon_get(it->icon, icon_px(t)) : 0;
            if (li) {
                ugfx_blit_alpha(s, cx, y + (h - li->h) / 2, li->w, li->h, li->px, li->w);
                cx += icon_px(t) + 6;
            }
            if (it->label) {
                ugfx_draw_string_clipped(s, cx, y + (h - ugfx_char_h()) / 2,
                                          x + w - cx, it->label, fg, bg);
                cx += ugfx_text_width(it->label);
            }
            if (it->flags & UUI_TB_MENU) {
                int ax = cx + 3, ay = y + h / 2 - 1;
                for (int r = 0; r < 3; r++)
                    ugfx_fill_rect(s, ax + r, ay + r, 7 - 2 * r, 1, fg);
            }
            continue;
        }
        const struct uimg *ico = icon_get(t->items[i].icon, icon_px(t));
        if (ico) {
            // No greying pass for a disabled icon -- the wash under it
            // is the state signal, one rule for every control here.
            ugfx_blit_alpha(s, x + (w - ico->w) / 2, y + (h - ico->h) / 2,
                             ico->w, ico->h, ico->px, ico->w);
        } else {
            // The icon cache's missing-file rule: a letter, never an error.
            char c[2] = { t->items[i].tip ? t->items[i].tip[0] : '?', 0 };
            ugfx_draw_string(s, x + (w - ugfx_text_width(c)) / 2,
                              y + (h - ugfx_char_h()) / 2, c, t->fg,
                              uui_state_bg(t->bg, st));
        }
    }
}

// The tooltip, painted by the ROUTER's overlay pass so it lands above
// whatever sits under the strip. Below the button, slid inward at the
// surface's edges -- a tip that clips is a tip nobody can read.
static void tb_draw_tip(struct ugfx_surface *s, const struct uui_toolbar *t) {
    if (!t->tip_shown || t->hot < 0 || !t->items[t->hot].tip) return;
    const char *tip = t->items[t->hot].tip;

    int w, h, bx, by, bw, bh;
    if (!tip_metrics(t, &w, &h, &bx, &by, &bw, &bh)) return;

    int x, y;
    struct ugfx_surface *ps = t->tip_popup ? uui_popup_surface(t->tip_popup) : 0;
    if (ps) {
        // Its own surface: the compositor placed it, so the tip sits at
        // that surface's origin and nothing here clamps.
        s = ps;
        x = y = 0;
    } else {
        // In-window fallback -- the only clamp this widget can do, and
        // the reason a tooltip near the window edge used to be cut off
        // rather than moved.
        x = bx;
        y = by + bh + UUI_TIP_GAP;
        if (x + w > s->w) x = s->w - w;
        if (x < 0) x = 0;
        if (y + h > s->h) y = by - h - UUI_TIP_GAP; // no room below: flip above
    }

    ugfx_fill_rect(s, x, y, w, h, t->tip_bg);
    ugfx_draw_rect(s, x, y, w, h, t->border);
    ugfx_draw_string_clipped(s, x + UUI_TIP_PAD, y + UUI_TIP_PAD,
                              w - UUI_TIP_PAD * 2, tip, t->tip_fg, t->tip_bg);
}

// --- the ops table ----------------------------------------------------

static void tb_natural_op(const void *w, int *out_w, int *out_h) {
    uui_toolbar_natural_size((const struct uui_toolbar *)w, out_w, out_h);
}

static void tb_geometry_op(void *w, int x, int y, int rw, int rh) {
    struct uui_toolbar *t = w;
    t->x = x; t->y = y; t->w = rw; t->h = rh;
}

static void tb_bounds_op(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_toolbar *t = w;
    if (x) *x = t->x;
    if (y) *y = t->y;
    if (out_w) *out_w = t->w;
    if (out_h) *out_h = t->h;
}

static void tb_draw_op(struct ugfx_surface *s, const void *w) {
    tb_draw(s, (const struct uui_toolbar *)w);
}

static void tb_overlay_op(struct ugfx_surface *s, const void *w) {
    tb_draw_tip(s, (const struct uui_toolbar *)w);
}

static int tb_hit_op(const void *w, int cx, int cy) {
    const struct uui_toolbar *t = w;
    // The WHOLE strip, so a press between buttons is consumed rather
    // than falling through to whatever is behind the bar.
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

static int tb_press_op(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_toolbar *t = w;
    t->armed = uui_toolbar_hit_item(t, cx, cy);
    return 1; // inside the strip: always consumed (and takes the grab)
}

static int tb_motion_op(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons; // armed vs hover is resolved at release, by position
    struct uui_toolbar *t = w;
    int over = uui_toolbar_hit_item(t, cx, cy);
    if (over == t->hot) return 0;
    t->hot = over;
    t->hot_since = sys_ticks();
    tip_hide(t);   // a new button means a new tip, and the old SURFACE goes
    return 1;
}

static int tb_release_op(void *w, int cx, int cy) {
    struct uui_toolbar *t = w;
    int over = uui_toolbar_hit_item(t, cx, cy);
    int hit = (t->armed >= 0 && over == t->armed);
    if (hit) t->committed = t->items[t->armed].code;
    t->armed = -1;
    return 1;
}

const struct uui_widget_ops uui_toolbar_ops = {
    .natural_size = tb_natural_op,
    .set_geometry = tb_geometry_op,
    .bounds       = tb_bounds_op,
    .draw         = tb_draw_op,
    .draw_overlay = tb_overlay_op,
    .hit          = tb_hit_op,
    .press        = tb_press_op,
    .motion       = tb_motion_op,
    .release      = tb_release_op,
};
