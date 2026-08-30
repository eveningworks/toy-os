// toolbar -- see ui/uui_toolbar.h for the contract (one item_flags
// callback shared with the menu bar, tooltips on the app's tick).
#include "ui/uui_toolbar.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "lib/icon_cache.h"
#include "rt/sys.h" // sys_ticks()
#include <string.h>

// Font-derived, per docs/gui-guidelines.md.
static int icon_px(void)  { return ugfx_char_h() + 4; }
static int btn_w(void)    { return icon_px() + 10; }
static int btn_h(void)    { return icon_px() + 8; }
static int sep_w(void)    { return 7; }

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

static int is_sep(const struct uui_toolbar_item *it) { return it->icon == 0; }

static unsigned flags_of(const struct uui_toolbar *t, int i) {
    if (i < 0 || i >= t->count || is_sep(&t->items[i])) return 0;
    return t->item_flags ? t->item_flags(t->items[i].code) : 0;
}

void uui_toolbar_natural_size(const struct uui_toolbar *t, int *out_w, int *out_h) {
    int w = 4;
    for (int i = 0; i < t->count; i++)
        w += is_sep(&t->items[i]) ? sep_w() : btn_w();
    if (out_w) *out_w = w + 4;
    if (out_h) *out_h = btn_h() + 4;
}

int uui_toolbar_item_rect(const struct uui_toolbar *t, int i,
                           int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= t->count) return 0;
    int ix = t->x + 4;
    for (int j = 0; j < i; j++)
        ix += is_sep(&t->items[j]) ? sep_w() : btn_w();
    if (x) *x = ix;
    if (y) *y = t->y + 2;
    if (w) *w = is_sep(&t->items[i]) ? sep_w() : btn_w();
    if (h) *h = btn_h();
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

int uui_toolbar_tick(struct uui_toolbar *t) {
    int want = t->hot >= 0 && t->items[t->hot].tip &&
                sys_ticks() - t->hot_since >= UUI_TOOLTIP_DELAY_TICKS;
    if (want == t->tip_shown) return 0;
    t->tip_shown = want;
    return 1;
}

static void tb_draw(struct ugfx_surface *s, const struct uui_toolbar *t) {
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, t->bg);
    ugfx_fill_rect(s, t->x, t->y + t->h - 1, t->w, 1, t->border);

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

        const struct uimg *ico = icon_get(t->items[i].icon, icon_px());
        if (ico) {
            // No greying pass for a disabled icon -- the wash under it
            // is the state signal, one rule for every control here.
            ugfx_blit_alpha(s, x + (w - ico->w) / 2, y + (h - ico->h) / 2,
                             ico->w, ico->h, ico->px, ico->w);
        } else {
            // The icon cache's missing-file rule: a letter, never an error.
            char c[2] = { t->items[i].tip ? t->items[i].tip[0] : '?', 0 };
            ugfx_draw_string(s, x + (w - ugfx_char_w()) / 2,
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

    int bx, by, bw, bh;
    uui_toolbar_item_rect(t, t->hot, &bx, &by, &bw, &bh);
    int pad = 4;
    int w = ugfx_text_width(tip) + pad * 2;
    int h = ugfx_char_h() + pad * 2;
    int x = bx;
    int y = by + bh + 3;
    if (x + w > s->w) x = s->w - w;
    if (x < 0) x = 0;
    if (y + h > s->h) y = by - h - 3; // no room below: flip above

    ugfx_fill_rect(s, x, y, w, h, t->tip_bg);
    ugfx_draw_rect(s, x, y, w, h, t->border);
    ugfx_draw_string_clipped(s, x + pad, y + pad, w - pad * 2, tip,
                              t->tip_fg, t->tip_bg);
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
    t->tip_shown = 0;
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
