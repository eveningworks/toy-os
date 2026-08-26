// The tab strip. See ui/uui_tabs.h for the contract; this is the
// arithmetic and the drawing.
#include "ui/uui_tabs.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"

// Every measurement here is font-derived, per docs/gui-guidelines.md:
// raising the desktop font size has to move the strip with it.
#define TAB_PAD_X   (ugfx_char_w())
#define TAB_MIN_W   (ugfx_char_w() * 6)
#define CLOSE_W     (ugfx_char_w() * 2)

int uui_tabs_height(void) {
    return ugfx_char_h() + ugfx_char_h() / 2;
}

void uui_tabs_init(struct uui_tabs *t, struct uui_tab *tabs, int count,
                    void *ctx) {
    t->x = t->y = t->w = t->h = 0;
    t->tabs = tabs;
    t->count = count;
    t->selected = 0;
    t->hovered = -1;
    t->hovered_close = 0;
    t->pressed = -1;
    t->pressed_close = 0;
    t->on_select = 0;
    t->on_close = 0;
    t->ctx = ctx;
}

void uui_tabs_set_geometry(struct uui_tabs *t, int x, int y, int w, int h) {
    t->x = x; t->y = y; t->w = w; t->h = h;
}

// The width one tab gets. EQUAL SHARES of the strip, clamped to a floor
// -- which is what every tabbed terminal does, and the reason is that a
// tab's width must not jump around as its title changes: a close box
// that moves while the pointer is travelling to it is unhittable.
//
// Below the floor the tabs are allowed to run off the right edge, and
// that is the honest failure: a strip of forty tabs cannot show them
// all, and a caller that allows forty needs a scroll of its own.
static int tab_width(const struct uui_tabs *t) {
    if (t->count <= 0) return 0;
    int w = t->w / t->count;
    return w < TAB_MIN_W ? TAB_MIN_W : w;
}

int uui_tabs_rect(const struct uui_tabs *t, int index,
                   int *x, int *y, int *w, int *h) {
    if (index < 0 || index >= t->count) return 0;
    int tw = tab_width(t);
    if (x) *x = t->x + index * tw;
    if (y) *y = t->y;
    if (w) *w = tw;
    if (h) *h = t->h;
    return 1;
}

// Where the close box of tab `index` sits. Its own function because
// hit-testing and drawing must agree to the pixel -- deriving it twice
// is how a close box ends up looking hittable somewhere it is not.
static int close_rect(const struct uui_tabs *t, int index,
                       int *x, int *y, int *w, int *h) {
    if (index < 0 || index >= t->count || !t->tabs[index].closable) return 0;
    int tx, ty, tw, th;
    if (!uui_tabs_rect(t, index, &tx, &ty, &tw, &th)) return 0;
    if (tw < TAB_MIN_W) return 0;
    if (x) *x = tx + tw - CLOSE_W - TAB_PAD_X / 2;
    if (y) *y = ty + (th - CLOSE_W) / 2;
    if (w) *w = CLOSE_W;
    if (h) *h = CLOSE_W;
    return 1;
}

void uui_tabs_natural_size(const struct uui_tabs *t, int *out_w, int *out_h) {
    int widest = TAB_MIN_W;
    for (int i = 0; i < t->count; i++) {
        int need = 2 * TAB_PAD_X + ugfx_text_width(t->tabs[i].label);
        if (t->tabs[i].closable) need += CLOSE_W + TAB_PAD_X;
        if (need > widest) widest = need;
    }
    if (out_w) *out_w = widest * (t->count > 0 ? t->count : 1);
    if (out_h) *out_h = uui_tabs_height();
}

void uui_tabs_select(struct uui_tabs *t, int index) {
    if (index < 0) index = 0;
    if (index >= t->count) index = t->count - 1;
    if (index == t->selected) return; // MOVED, not merely re-asserted
    t->selected = index;
    if (t->on_select) t->on_select(t->ctx, index);
}

// Which tab, and whether the point is on its close box. -1 for neither.
static int tab_at(const struct uui_tabs *t, int cx, int cy, int *on_close) {
    if (on_close) *on_close = 0;
    if (!uui_hit(t->x, t->y, t->w, t->h, cx, cy)) return -1;
    int tw = tab_width(t);
    if (tw <= 0) return -1;
    int idx = (cx - t->x) / tw;
    if (idx < 0 || idx >= t->count) return -1;
    int bx, by, bw, bh;
    if (on_close && close_rect(t, idx, &bx, &by, &bw, &bh))
        *on_close = uui_hit(bx, by, bw, bh, cx, cy);
    return idx;
}

static void draw_close(struct ugfx_surface *s, int x, int y, int w, int h,
                        uint32_t ink) {
    // Two strokes rather than a glyph: the font has no multiplication
    // sign, and an 'x' at this size reads as a letter.
    int n = w < h ? w : h;
    for (int i = 1; i < n - 1; i++) {
        ugfx_fill_rect(s, x + i, y + i, 1, 1, ink);
        ugfx_fill_rect(s, x + (n - 1 - i), y + i, 1, 1, ink);
    }
}

static void draw_one(struct ugfx_surface *s, const struct uui_tabs *t, int i) {
    int x, y, w, h;
    if (!uui_tabs_rect(t, i, &x, &y, &w, &h)) return;

    int is_sel = (i == t->selected);
    enum uui_state st = UUI_STATE_REST;
    if (t->pressed == i && !t->pressed_close) st = UUI_STATE_PRESSED;
    else if (t->hovered == i && !is_sel)      st = UUI_STATE_HOVER;

    // SELECTION OUTRANKS HOVER, which is why the hover above is skipped
    // for the selected tab: a hover shift on top of the selected fill
    // is a two-unit change nobody can see, and a test measuring it
    // measures nothing (docs/gui-guidelines.md).
    uint32_t base = is_sel ? UTHEME_WINDOW_BG : UTHEME_PANEL_BG;
    uint32_t ink  = UTHEME_TEXT;
    ugfx_fill_rect(s, x, y, w, h, uui_state_bg(base, st));

    // The selected tab is joined to the page below it and separated
    // from its neighbours -- the border is drawn on three sides only,
    // which is what makes a strip read as tabs rather than buttons.
    ugfx_fill_rect(s, x, y, 1, h, UTHEME_BORDER);
    ugfx_fill_rect(s, x + w - 1, y, 1, h, UTHEME_BORDER);
    ugfx_fill_rect(s, x, y, w, 1, UTHEME_BORDER);
    if (!is_sel) ugfx_fill_rect(s, x, y + h - 1, w, 1, UTHEME_BORDER);
    if (is_sel)  ugfx_fill_rect(s, x, y, w, 2, UTHEME_ACCENT);

    int bx, by, bw, bh;
    int has_close = close_rect(t, i, &bx, &by, &bw, &bh);
    int text_w = w - 2 * TAB_PAD_X - (has_close ? CLOSE_W + TAB_PAD_X : 0);
    if (text_w > 0) {
        // CLIPPED, never plain: a title from a shell is arbitrary text
        // and gfx_draw_string() does not clip (docs/gui-guidelines.md).
        ugfx_draw_string_clipped(s, x + TAB_PAD_X,
                                  y + (h - ugfx_char_h()) / 2 + 1,
                                  text_w, t->tabs[i].label, ink,
                                  UGFX_TRANSPARENT);
    }
    if (has_close) {
        uint32_t cink = (t->hovered == i && t->hovered_close)
                          ? UTHEME_ACCENT : ink;
        draw_close(s, bx, by, bw, bh, cink);
    }
}

// --- the ops table ---------------------------------------------------

static void ops_natural(const void *w, int *ow, int *oh) {
    uui_tabs_natural_size((const struct uui_tabs *)w, ow, oh);
}

static void ops_geometry(void *w, int x, int y, int width, int height) {
    uui_tabs_set_geometry((struct uui_tabs *)w, x, y, width, height);
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_tabs *t = (const struct uui_tabs *)w;
    if (x)  *x  = t->x;
    if (y)  *y  = t->y;
    if (ow) *ow = t->w;
    if (oh) *oh = t->h;
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_tabs *t = (const struct uui_tabs *)w;
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, UTHEME_PANEL_BG);
    for (int i = 0; i < t->count; i++) draw_one(s, t, i);
}

static int ops_hit(const void *w, int cx, int cy) {
    const struct uui_tabs *t = (const struct uui_tabs *)w;
    // A BOOLEAN, and this is the rule CLAUDE.md keeps: returning the
    // index would make tab 0 -- the one whose index is falsey -- report
    // as not hit, so the first tab silently could not be clicked.
    return tab_at(t, cx, cy, 0) >= 0;
}

static int ops_press(void *w, int cx, int cy) {
    struct uui_tabs *t = (struct uui_tabs *)w;
    int on_close = 0;
    int i = tab_at(t, cx, cy, &on_close);
    if (i < 0) return 0;
    t->pressed = i;
    t->pressed_close = on_close;
    // ARMED, NOT COMMITTED. Selection happens on release with the
    // close, so that a press dragged off its tab does nothing --
    // docs/gui-guidelines.md's press/release rule, and it matters most
    // for the close box because that one destroys something.
    return 1;
}

static int ops_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_tabs *t = (struct uui_tabs *)w;
    int on_close = 0;
    int i = tab_at(t, cx, cy, &on_close);
    int changed = (i != t->hovered) || (on_close != t->hovered_close);
    t->hovered = i;
    t->hovered_close = on_close;
    return changed;
}

static int ops_release(void *w, int cx, int cy) {
    struct uui_tabs *t = (struct uui_tabs *)w;
    int armed = t->pressed, armed_close = t->pressed_close;
    t->pressed = -1;
    t->pressed_close = 0;
    if (armed < 0) return 0;

    int on_close = 0;
    int i = tab_at(t, cx, cy, &on_close);
    // The release must land on the SAME tab, and on the same half of
    // it, as the press. Anything else is a cancelled click.
    if (i != armed) return 1;
    if (armed_close) {
        if (on_close && t->on_close) t->on_close(t->ctx, i);
        return 1;
    }
    uui_tabs_select(t, i);
    return 1;
}

const struct uui_widget_ops uui_tabs_ops = {
    .natural_size = ops_natural,
    .set_geometry = ops_geometry,
    .bounds       = ops_bounds,
    .draw         = ops_draw,
    .hit          = ops_hit,
    .press        = ops_press,
    .motion       = ops_motion,
    .release      = ops_release,
};
