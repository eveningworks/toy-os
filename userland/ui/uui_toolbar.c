// toolbar -- see ui/uui_toolbar.h for the contract (one item_flags
// callback shared with the menu bar, tooltips on the app's tick).
#include "ui/uui_toolbar.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "ui/uui_popup.h"
#include "ui/utheme.h"
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
    if (it->flags & UUI_TB_TEXT) return 12 + (it->label ? ugfx_text_width(it->label) : 0);
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
    t->more = (struct uui_toolbar_item){ "tb-more", "See more", -1, 0, UUI_TB_MENU, 0, 0 };
    uui_menubar_init(&t->menu, 0, 0);
}

// Item `i`, where `count` names the overflow button.
static const struct uui_toolbar_item *item_at(const struct uui_toolbar *t, int i) {
    return i == t->count ? &t->more : &t->items[i];
}

static int span_w(const struct uui_toolbar *t, int from, int to) {
    int w = 0;
    for (int j = from; j < to; j++) w += item_w(t, &t->items[j]);
    return w;
}

// One pass: the longest prefix that fits beside the More button, once
// everything together does not fit without it.
int uui_toolbar_shown(const struct uui_toolbar *t) {
    if (!t->overflow) return t->count;
    int room = t->w - 8 - item_w(t, &t->more), used = 0, n = 0, fit = -1;
    for (; n < t->count; n++) {
        used += item_w(t, &t->items[n]);
        if (fit < 0 && used > room) fit = n;
    }
    if (used <= t->w - 8 || fit < 0) return t->count;
    n = fit;
    while (n > 0 && is_sep_item(&t->items[n - 1])) n--;   // no dangling separator
    return n;
}

void uui_toolbar_set_bounds(struct uui_toolbar *t, int x, int y, int w, int h) {
    uui_menubar_set_bounds(&t->menu, x, y, w, h);
}

static int is_sep(const struct uui_toolbar_item *it) { return is_sep_item(it); }

static unsigned flags_of(const struct uui_toolbar *t, int i) {
    if (i < 0 || i >= t->count || is_sep(&t->items[i])) return 0;
    if (t->items[i].flags & UUI_TB_TEXT) return 0;
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
    if (i < 0 || i > t->count) return 0;
    int n = uui_toolbar_shown(t);
    if (i == t->count && n == t->count) return 0;   // no overflow button
    int mw = n < t->count ? item_w(t, &t->more) : 0;
    int right = t->x + t->w - 4 - mw;               // the groups end here
    int ix, iw = item_w(t, item_at(t, i)), ih = btn_h(t);
    if (i == t->count) {
        ix = right;
    } else if (i >= n) {
        ix = right;             // folded: ZERO WIDTH under the More button,
        iw = 0;                 // so a popup anchored at it opens there
    } else {
        int e = end_group(t);
        if (e > n) e = n;
        ix = t->x + 4;
        int from = 0;
        if (i >= e) {
            // Packed against the right edge, never over the left group.
            ix = right - span_w(t, e, n);
            if (ix < t->x + 4 + span_w(t, 0, e)) ix = t->x + 4 + span_w(t, 0, e);
            from = e;
        }
        ix += span_w(t, from, i);
    }
    if (x) *x = ix;
    if (y) *y = t->y + (t->compact ? 0 : 2);
    if (w) *w = iw;
    if (h) *h = ih;
    return 1;
}

int uui_toolbar_hit_item(const struct uui_toolbar *t, int cx, int cy) {
    for (int i = 0; i <= t->count; i++) {
        int x, y, w, h;
        if (!uui_toolbar_item_rect(t, i, &x, &y, &w, &h)) continue;
        if (!uui_hit(x, y, w, h, cx, cy)) continue;
        if (i == t->count) return i;
        if (is_sep(&t->items[i]) || (t->items[i].flags & UUI_TB_TEXT)) return -1;
        if (flags_of(t, i) & UUI_MI_DISABLED) return -1;
        return i;
    }
    return -1;
}

// The hidden items as menu rows: a label (or the tip, for an icon-only
// button), its accel and its code. Readouts are left out, and separators
// survive only BETWEEN rows.
static void open_more(struct uui_toolbar *t) {
    int k = 0;
    for (int i = uui_toolbar_shown(t); i < t->count && k < UUI_TOOLBAR_MENU_MAX; i++) {
        const struct uui_toolbar_item *it = &t->items[i];
        if (it->flags & UUI_TB_TEXT) continue;
        if (!is_sep(it) && !it->label && !it->tip) continue;   // nothing to call the row
        if (is_sep(it)) {
            if (k && t->menu_items[k - 1].label)
                t->menu_items[k++] = (struct uui_menu_item)UUI_MENU_SEP;
            continue;
        }
        t->menu_items[k++] = (struct uui_menu_item)UUI_MENU_ICON(
            it->label ? it->label : it->tip, it->code, it->accel, it->icon, it->tint);
    }
    while (k && !t->menu_items[k - 1].label) k--;
    if (!k) return;
    int x, y, w, h;
    uui_toolbar_item_rect(t, t->count, &x, &y, &w, &h);
    t->menu.item_flags = t->item_flags;
    uui_menubar_open_at(&t->menu, t->menu_items, k, x, y + h);
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
    if (t->hot < 0 || !item_at(t, t->hot)->tip) return 0;
    if (!uui_toolbar_item_rect(t, t->hot, bx, by, bw, bh) || *bw == 0) return 0;
    *w = ugfx_text_width(item_at(t, t->hot)->tip) + UUI_TIP_PAD * 2;
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
    // tip_metrics() also refuses a hot item no longer in the strip.
    int w, h, bx, by, bw, bh;
    int want = !uui_menubar_is_open(&t->menu) &&
                tip_metrics(t, &w, &h, &bx, &by, &bw, &bh) &&
                sys_ticks() - t->hot_since >= UUI_TOOLTIP_DELAY_TICKS;
    if (want == t->tip_shown) return 0;
    if (!want) { tip_hide(t); return 1; }

    t->tip_shown = 1;
    {
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

// The item's icon: its own ink, or its tint as a symbolic icon -- in the
// accent's text colour when latched in the accent, where a coloured
// glyph on the accent fill would lose its contrast.
// A DISABLED ICON IS GREYED ITSELF, in the colour a disabled label takes
// -- its action tint dropped -- as Windows and Breeze grey theirs: the
// wash under the item alone barely shows on this chrome, and a coloured
// icon read as live.
static void draw_icon(struct ugfx_surface *s, const struct uui_toolbar *t,
                      const struct uui_toolbar_item *it,
                      const struct uimg *ico, int x, int y, int accent, int disabled) {
    if (disabled) {
        ugfx_blit_tinted(s, x, y, ico->w, ico->h, ico->px, ico->w,
                         uui_state_bg(t->fg, UUI_STATE_DISABLED));
        return;
    }
    if (it->tint) {
        uint32_t c = it->tint < UTHEME_ACT_COUNT ? utheme_action((int)it->tint) : it->tint;
        ugfx_blit_tinted(s, x, y, ico->w, ico->h, ico->px, ico->w,
                         accent ? UTHEME_ACCENT_TEXT : c);
    }
    else
        ugfx_blit_alpha(s, x, y, ico->w, ico->h, ico->px, ico->w);
}

static void tb_draw(struct ugfx_surface *s, const struct uui_toolbar *t) {
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, t->bg);
    if (!t->compact) ugfx_fill_rect(s, t->x, t->y + t->h - 1, t->w, 1, t->border);

    for (int i = 0; i <= t->count; i++) {
        int x, y, w, h;
        if (!uui_toolbar_item_rect(t, i, &x, &y, &w, &h) || w == 0) continue;
        const struct uui_toolbar_item *it = item_at(t, i);
        if (it->flags & UUI_TB_TEXT) {
            if (it->label)
                ugfx_draw_string_clipped(s, x + 6, y + (h - ugfx_char_h()) / 2,
                                          w - 6, it->label, t->fg, t->bg);
            continue;
        }
        if (is_sep(it)) {
            ugfx_fill_rect(s, x + sep_w() / 2, y + 3, 1, h - 6, t->border);
            continue;
        }
        unsigned f = flags_of(t, i);

        // A LATCHED toggle draws pressed-in -- or, with `accent_latch`,
        // filled in the accent, which becomes the base the washes shift
        // from. Pressing wins over hover, uui_button's own ordering, and
        // all washes come from uui_state_bg so hover darkens on this
        // theme like everything else.
        int accent = t->accent_latch && (f & UUI_MI_CHECKED) && !(f & UUI_MI_DISABLED);
        uint32_t base = accent ? UTHEME_ACCENT : t->bg;
        uint32_t ink = accent ? UTHEME_ACCENT_TEXT : t->fg;
        enum uui_state st = UUI_STATE_REST;
        if (f & UUI_MI_DISABLED) st = UUI_STATE_DISABLED;
        else if (i == t->armed || ((f & UUI_MI_CHECKED) && !accent)) st = UUI_STATE_PRESSED;
        else if (i == t->hot)    st = UUI_STATE_HOVER;
        if (i == t->count && uui_menubar_is_open(&t->menu)) st = UUI_STATE_PRESSED;
        uint32_t bg = uui_state_bg(base, st);
        // Inset by a pixel when filled, or two latched neighbours read
        // as one wide button.
        if (accent)                     ugfx_fill_rect(s, x + 1, y, w - 2, h, bg);
        else if (st != UUI_STATE_REST)  ugfx_fill_rect(s, x, y, w, h, bg);
        if ((f & UUI_MI_CHECKED) && !accent)
            ugfx_draw_rect(s, x, y, w, h, t->border);

        if (it->label || (it->flags & UUI_TB_MENU)) {
            // Icon, word, chevron, left to right.
            uint32_t fg = (f & UUI_MI_DISABLED) ? uui_state_bg(t->fg, UUI_STATE_DISABLED) : ink;
            int cx = x + 6;
            const struct uimg *li = it->icon ? icon_get(it->icon, icon_px(t)) : 0;
            if (li) {
                draw_icon(s, t, it, li, cx, y + (h - li->h) / 2, accent,
                          (f & UUI_MI_DISABLED) != 0);
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
        const struct uimg *ico = icon_get(it->icon, icon_px(t));
        if (ico) {
            draw_icon(s, t, it, ico, x + (w - ico->w) / 2, y + (h - ico->h) / 2, accent,
                      (f & UUI_MI_DISABLED) != 0);
        } else {
            // The icon cache's missing-file rule: a letter, never an error.
            char c[2] = { it->tip ? it->tip[0] : '?', 0 };
            ugfx_draw_string(s, x + (w - ugfx_text_width(c)) / 2,
                              y + (h - ugfx_char_h()) / 2, c, ink, bg);
        }
    }
}

// The tooltip, painted by the ROUTER's overlay pass so it lands above
// whatever sits under the strip. Below the button, slid inward at the
// surface's edges -- a tip that clips is a tip nobody can read.
static void tb_draw_tip(struct ugfx_surface *s, const struct uui_toolbar *t) {
    if (!t->tip_shown || t->hot < 0 || !item_at(t, t->hot)->tip) return;
    const char *tip = item_at(t, t->hot)->tip;

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
    // A resize reshuffles what is hidden; a menu listing the old set
    // would commit items that are now back in the strip.
    if (rw != t->w) {
        uui_menubar_close(&t->menu);
        t->hot = -1;            // the item under a still pointer may have moved
        tip_hide(t);
    }
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
    const struct uui_toolbar *t = w;
    tb_draw_tip(s, t);
    uui_menubar_draw_popup(s, &t->menu);
}

static int tb_overlay_active_op(const void *w) {
    return uui_menubar_is_open(&((const struct uui_toolbar *)w)->menu);
}

static int tb_hit_op(const void *w, int cx, int cy) {
    const struct uui_toolbar *t = w;
    // The WHOLE strip, so a press between buttons is consumed rather
    // than falling through to whatever is behind the bar.
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

// With the menu open every press arrives here first (overlay_active) and
// is the MENU's: a row arms, anything else dismisses -- including a press
// on the overflow button, which therefore toggles the menu shut.
static int tb_press_op(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_toolbar *t = w;
    // Set by EVERY press, so a grab dropped before its release cannot
    // leave the next ordinary click routed to a closed menu.
    t->menu_press = uui_menubar_is_open(&t->menu);
    if (t->menu_press) {
        t->armed = -1;
        uui_menubar_press(&t->menu, cx, cy);
        return 1;
    }
    t->armed = uui_toolbar_hit_item(t, cx, cy);
    return 1; // inside the strip: always consumed (and takes the grab)
}

static int tb_motion_op(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons; // armed vs hover is resolved at release, by position
    struct uui_toolbar *t = w;
    if (uui_menubar_is_open(&t->menu)) return uui_menubar_motion(&t->menu, cx, cy);
    int over = uui_toolbar_hit_item(t, cx, cy);
    if (over == t->hot) return 0;
    t->hot = over;
    t->hot_since = sys_ticks();
    tip_hide(t);   // a new button means a new tip, and the old SURFACE goes
    return 1;
}

// The overflow button OPENS on release, like any button here -- not on
// press as a menu bar title does, because this gesture's release would
// then land on whichever row opened under the pointer.
static int tb_release_op(void *w, int cx, int cy) {
    struct uui_toolbar *t = w;
    if (t->menu_press) {
        t->menu_press = 0;
        int code = uui_menubar_release(&t->menu, cx, cy);
        if (code >= 0) t->committed = code;
        return 1;
    }
    int over = uui_toolbar_hit_item(t, cx, cy);
    int hit = (t->armed >= 0 && over == t->armed);
    if (hit && t->armed == t->count) {
        tip_hide(t);
        t->hot = -1;            // motion goes to the menu now, not the strip
        open_more(t);
    } else if (hit) {
        t->committed = t->items[t->armed].code;
    }
    t->armed = -1;
    return 1;
}

// Only while the menu is open: the router hands an open overlay the key
// wherever focus is, and the menu walks, commits or closes on it.
static int tb_key_op(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_toolbar *t = w;
    if (!uui_menubar_is_open(&t->menu)) return 0;
    int code;
    uui_menubar_key(&t->menu, key, &code);
    if (code >= 0) t->committed = code;
    return 1;
}

static int tb_accepts_focus_op(const void *w) {
    (void)w;
    return 0;   // it takes keys only through its open menu, never as focus
}

// An OVERFLOWING strip only (the others' logs predate this): button i for
// each one in the strip, more, shown, and the open menu's rows in
// uui_menubar's own vocabulary (popup 0, item 0 i, open).
static void tb_describe_op(const void *w, const struct uui_describe *d) {
    const struct uui_toolbar *t = w;
    if (!t->overflow) return;
    int x, y, iw, ih;
    for (int i = 0; i < t->count; i++)
        if (uui_toolbar_item_rect(t, i, &x, &y, &iw, &ih) && iw > 0)
            uui_describe_rect_i(d, "button", i, x, y, iw, ih);
    if (uui_toolbar_item_rect(t, t->count, &x, &y, &iw, &ih))
        uui_describe_rect(d, "more", x, y, iw, ih);
    uui_describe_int(d, "shown", uui_toolbar_shown(t));
    uui_menubar_ops.describe(&t->menu, d);
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
    .key          = tb_key_op,
    .accepts_focus = tb_accepts_focus_op,
    .overlay_active = tb_overlay_active_op,
    .describe     = tb_describe_op,
};
