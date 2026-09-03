// The tab strip. See ui/uui_tabs.h for the contract; this is the
// arithmetic and the drawing.
#include "ui/uui_tabs.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"

// Every measurement here is font-derived, per docs/gui-guidelines.md:
// raising the desktop font size has to move the strip with it.
#define TAB_PAD_X   (ugfx_char_w())
#define TAB_MIN_W   (ugfx_char_w() * 6)
#define TAB_MAX_W   (ugfx_char_w() * 24)   // Windows Terminal caps a tab about here
#define CLOSE_W     (ugfx_char_w() * 2)
#define NEW_W       (ugfx_char_w() * 3)

// The selected tab's top corners. A third of the line height reads as a
// deliberate curve at every font size the desktop offers; below three
// pixels a round is indistinguishable from a chamfer.
static int corner_r(void) {
    int r = ugfx_char_h() / 3;
    return r < 3 ? 3 : r;
}

int uui_tabs_height(void) {
    return ugfx_char_h() + ugfx_char_h() / 2;
}

void uui_tabs_init(struct uui_tabs *t, struct uui_tab *tabs, int count,
                    void *ctx) {
    t->x = t->y = t->w = t->h = 0;
    t->tabs = tabs;
    t->count = count;
    t->selected = 0;
    t->show_new = 0;
    t->numbered = 0;
    t->hovered = -1;
    t->hovered_close = 0;
    t->hovered_new = 0;
    t->pressed = -1;
    t->pressed_close = 0;
    t->pressed_new = 0;
    t->frozen_n = 0;
    t->on_select = 0;
    t->on_close = 0;
    t->on_new = 0;
    t->ctx = ctx;
}

void uui_tabs_set_geometry(struct uui_tabs *t, int x, int y, int w, int h) {
    t->x = x; t->y = y; t->w = w; t->h = h;
}

// How much of the strip the tabs get. The "+" is subtracted FIRST, which
// is what pins it: sharing the whole strip and then drawing over the
// right-hand tab would put a button on top of that tab's close box.
static int tabs_area_w(const struct uui_tabs *t) {
    int w = t->w - (t->show_new ? NEW_W : 0);
    return w < 0 ? 0 : w;
}

int uui_tabs_new_rect(const struct uui_tabs *t, int *x, int *y, int *w, int *h) {
    if (!t->show_new) return 0;
    if (x) *x = t->x + tabs_area_w(t);
    if (y) *y = t->y;
    if (w) *w = NEW_W;
    if (h) *h = t->h;
    return 1;
}

static void number_prefix(const struct uui_tabs *t, int index, char *out,
                           int cap);

// What tab `i` wants: padding, number, title and close box, capped.
static int natural_w(const struct uui_tabs *t, int i) {
    char pre[6];
    number_prefix(t, i, pre, sizeof pre);
    int w = 2 * TAB_PAD_X + ugfx_text_width(pre) + ugfx_text_width(t->tabs[i].label);
    if (t->tabs[i].closable) w += CLOSE_W + TAB_PAD_X;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    if (w < TAB_MIN_W) w = TAB_MIN_W;
    return w;
}

// The width tab `i` gets. Natural and packed left while the naturals
// fit; EQUAL SHARES with a floor once they do not, because a strip of
// forty tabs cannot show them all and a share keeps each one clickable
// for longer than a cap does. Below the floor the tabs run off the
// right edge, and that is the honest failure.
//
// While frozen (see the header) the answer is the frozen table, which
// is what keeps a close box still under a pointer heading for it.
static int tab_width_at(const struct uui_tabs *t, int i) {
    if (t->count <= 0 || i < 0 || i >= t->count) return 0;
    if (t->frozen_n == t->count && t->frozen_n > 0) return t->frozen_w[i];
    int area = tabs_area_w(t), total = 0;
    for (int k = 0; k < t->count; k++) total += natural_w(t, k);
    if (total <= area) return natural_w(t, i);
    int w = area / t->count;
    return w < TAB_MIN_W ? TAB_MIN_W : w;
}

static void freeze(struct uui_tabs *t) {
    if (t->count <= 0 || t->count > UUI_TABS_FREEZE_MAX) { t->frozen_n = 0; return; }
    if (t->frozen_n == t->count) return;
    for (int i = 0; i < t->count; i++) t->frozen_w[i] = tab_width_at(t, i);
    t->frozen_n = t->count;
}

int uui_tabs_rect(const struct uui_tabs *t, int index,
                   int *x, int *y, int *w, int *h) {
    if (index < 0 || index >= t->count) return 0;
    int tx = t->x;
    for (int i = 0; i < index; i++) tx += tab_width_at(t, i);
    if (x) *x = tx;
    if (y) *y = t->y;
    if (w) *w = tab_width_at(t, index);
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
    if (tw < TAB_MIN_W) return 0;   // shares below the floor have no room for one
    if (x) *x = tx + tw - CLOSE_W - TAB_PAD_X / 2;
    if (y) *y = ty + (th - CLOSE_W) / 2;
    if (w) *w = CLOSE_W;
    if (h) *h = CLOSE_W;
    return 1;
}

// "N: " for tab `index`, or empty. ONE TAB IS NOT NUMBERED -- a lone
// "1:" is three characters saying nothing.
static void number_prefix(const struct uui_tabs *t, int index, char *out,
                           int cap) {
    out[0] = '\0';
    if (!t->numbered || t->count < 2 || cap < 6) return;
    int n = index + 1;
    int i = 0;
    if (n >= 10) out[i++] = (char)('0' + (n / 10) % 10);
    out[i++] = (char)('0' + n % 10);
    out[i++] = ':';
    out[i++] = ' ';
    out[i] = '\0';
}

void uui_tabs_natural_size(const struct uui_tabs *t, int *out_w, int *out_h) {
    int total = 0;
    for (int i = 0; i < t->count; i++) total += natural_w(t, i);
    if (t->count == 0) total = TAB_MIN_W;
    if (out_w) *out_w = total + (t->show_new ? NEW_W : 0);
    if (out_h) *out_h = uui_tabs_height();
}

void uui_tabs_select(struct uui_tabs *t, int index) {
    if (index < 0) index = 0;
    if (index >= t->count) index = t->count - 1;
    if (index == t->selected) return; // MOVED, not merely re-asserted
    t->selected = index;
    if (t->on_select) t->on_select(t->ctx, index);
}

static int hit_new(const struct uui_tabs *t, int cx, int cy) {
    int x, y, w, h;
    return uui_tabs_new_rect(t, &x, &y, &w, &h) && uui_hit(x, y, w, h, cx, cy);
}

// Which tab, and whether the point is on its close box. -1 for neither.
static int tab_at(const struct uui_tabs *t, int cx, int cy, int *on_close) {
    if (on_close) *on_close = 0;
    if (!uui_hit(t->x, t->y, t->w, t->h, cx, cy)) return -1;
    if (hit_new(t, cx, cy)) return -1;   // the "+" is not tab N + 1
    int idx = -1;
    for (int i = 0, tx = t->x; i < t->count; i++) {
        int tw = tab_width_at(t, i);
        if (cx >= tx && cx < tx + tw) { idx = i; break; }
        tx += tw;
    }
    if (idx < 0) return -1;
    int bx, by, bw, bh;
    if (on_close && close_rect(t, idx, &bx, &by, &bw, &bh))
        *on_close = uui_hit(bx, by, bw, bh, cx, cy);
    return idx;
}

// --- drawing ----------------------------------------------------------

// A rect whose TOP corners are rounded. Kept static rather than promoted
// to ugfx: one caller, which is this project's bar for a shared helper.
//
// The arc is tested at PIXEL CENTRES against radius r, which is what
// makes the inset sequence read as a curve (2,1,0,0 at r=4) rather than
// as a staircase; testing corners instead cuts a full r off the top row.
static void fill_top_rounded(struct ugfx_surface *s, int x, int y, int w,
                              int h, int r, uint32_t c) {
    if (r < 1 || w < 2 * r || h < r) { ugfx_fill_rect(s, x, y, w, h, c); return; }
    for (int dy = 0; dy < r; dy++) {
        int ay = 2 * r - 2 * dy - 1;
        int inset = 0;
        while (inset < r) {
            int ax = 2 * r - 2 * inset - 1;
            if (ax * ax + ay * ay <= 4 * r * r) break;
            inset++;
        }
        ugfx_fill_rect(s, x + inset, y + dy, w - 2 * inset, 1, c);
    }
    ugfx_fill_rect(s, x, y + r, w, h - r, c);
}

// Two strokes rather than a glyph: the font has no multiplication sign,
// and an 'x' at this size reads as a letter.
//
// **THE TARGET IS GENEROUS AND THE MARK IS NOT.** Drawing corner to
// corner of the hit box made a close mark nearly as tall as the strip,
// crowding the label beside it; every real tab puts a small mark in a
// large target, and only the target has to be easy to hit.
static void draw_close(struct ugfx_surface *s, int x, int y, int w, int h,
                        uint32_t ink) {
    int box = w < h ? w : h;
    int n = ugfx_char_h() / 2;
    if (n < 5) n = 5;
    if (n > box) n = box;
    int gx = x + (w - n) / 2, gy = y + (h - n) / 2;
    for (int i = 0; i < n; i++) {
        ugfx_fill_rect(s, gx + i, gy + i, 1, 1, ink);
        ugfx_fill_rect(s, gx + (n - 1 - i), gy + i, 1, 1, ink);
    }
}

// The "+", drawn for the same reason the close box is: the font's '+'
// is a text glyph on a text baseline, and this one has to sit on the
// button's centre.
static void draw_plus(struct ugfx_surface *s, int cx, int cy, int n,
                       uint32_t ink) {
    // A CROSS CENTRES ON A PIXEL, so the arm runs from cx-h to cx+h
    // inclusive -- an odd span, always centred. Halving an even width
    // instead puts it a pixel left of the close boxes beside it.
    int half = n / 2;
    ugfx_fill_rect(s, cx - half, cy, 2 * half + 1, 1, ink);
    ugfx_fill_rect(s, cx, cy - half, 1, 2 * half + 1, ink);
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
    uint32_t ink = UTHEME_TEXT;
    if (is_sel) {
        // **THE ACCENT IS ON TOP**, against the light chrome. On the
        // bottom edge it is a thin line over a terminal's black page,
        // which is where it has the least contrast of anywhere it could
        // be. Inset by the corner radius so it follows the rounding
        // instead of overhanging it.
        fill_top_rounded(s, x, y, w, h, corner_r(), UTHEME_WHITE);
        ugfx_fill_rect(s, x + corner_r(), y, w - 2 * corner_r(), 2,
                        UTHEME_ACCENT);
    } else {
        // RECESSED, not bare: TAB_REST is darker than the strip ground,
        // so the strip reads as wells with one tab raised out of them.
        fill_top_rounded(s, x, y, w, h, corner_r(),
                          uui_state_bg(UTHEME_TAB_REST, st));
        // A HAIRLINE, NOT A BORDER. Separators only between two resting
        // tabs: one beside the selected tab would land against that
        // tab's own rounded edge and read as a stray mark.
        int right_sel = (i + 1 == t->selected);
        if (i + 1 < t->count && !right_sel)
            ugfx_fill_rect(s, x + w - 1, y + h / 4, 1, h / 2, UTHEME_BORDER);
    }

    int bx, by, bw, bh;
    int has_close = close_rect(t, i, &bx, &by, &bw, &bh);
    int text_w = w - 2 * TAB_PAD_X - (has_close ? CLOSE_W + TAB_PAD_X : 0);
    int tx = x + TAB_PAD_X, ty = y + (h - ugfx_char_h()) / 2 + 1;

    // The number goes first and the label gets what is left -- so a long
    // title clips and the number, which is the part that tells the tabs
    // apart, never does.
    char pre[6];
    number_prefix(t, i, pre, sizeof pre);
    if (pre[0] && text_w > 0) {
        int pw = ugfx_text_width(pre);
        ugfx_draw_string_clipped(s, tx, ty, text_w, pre, ink, UGFX_TRANSPARENT);
        tx += pw;
        text_w -= pw;
    }
    if (text_w > 0) {
        // CLIPPED, never plain: a title from a shell is arbitrary text
        // and gfx_draw_string() does not clip (docs/gui-guidelines.md).
        ugfx_draw_string_clipped(s, tx, ty, text_w, t->tabs[i].label, ink,
                                  UGFX_TRANSPARENT);
    }
    if (has_close) {
        uint32_t cink = (t->hovered == i && t->hovered_close)
                          ? UTHEME_ACCENT : ink;
        draw_close(s, bx, by, bw, bh, cink);
    }
}

static void draw_new(struct ugfx_surface *s, const struct uui_tabs *t) {
    int x, y, w, h;
    if (!uui_tabs_new_rect(t, &x, &y, &w, &h)) return;
    enum uui_state st = UUI_STATE_REST;
    if (t->pressed_new)      st = UUI_STATE_PRESSED;
    else if (t->hovered_new) st = UUI_STATE_HOVER;
    if (st != UUI_STATE_REST)
        fill_top_rounded(s, x + 1, y + 1, w - 2, h - 1, corner_r(),
                          uui_state_bg(UTHEME_PANEL_BG, st));
    draw_plus(s, x + w / 2, y + h / 2, ugfx_char_w(), UTHEME_TEXT);
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
    // THE STRIP GROUND IS DARKER THAN THE SELECTED TAB, which is what
    // makes the selection a LIFT rather than a tint: panel over field is
    // ten units and would be the "moved the background by two out of
    // 255" trap docs/gui-guidelines.md names.
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, UTHEME_WINDOW_BG);
    // THE BASELINE IS WHAT THE SELECTED TAB BREAKS. Drawn under every
    // tab and then painted over by that tab's accent, so the break is a
    // consequence of the fill rather than a second calculation that has
    // to agree with it.
    ugfx_fill_rect(s, t->x, t->y + t->h - 1, t->w, 1, UTHEME_BORDER);
    for (int i = 0; i < t->count; i++) draw_one(s, t, i);
    draw_new(s, t);
}

static int ops_hit(const void *w, int cx, int cy) {
    const struct uui_tabs *t = (const struct uui_tabs *)w;
    // A BOOLEAN, and this is the rule CLAUDE.md keeps: returning the
    // index would make tab 0 -- the one whose index is falsey -- report
    // as not hit, so the first tab silently could not be clicked.
    return tab_at(t, cx, cy, 0) >= 0 || hit_new(t, cx, cy);
}

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_tabs *t = (struct uui_tabs *)w;
    if (hit_new(t, cx, cy)) { t->pressed_new = 1; return 1; }
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
    int on_new = hit_new(t, cx, cy);
    // FREEZE ON ENTRY, THAW ON EXIT. Inside the strip a title change
    // must not move a close box; outside it nobody is aiming at one.
    if (uui_hit(t->x, t->y, t->w, t->h, cx, cy)) freeze(t);
    else t->frozen_n = 0;
    int changed = (i != t->hovered) || (on_close != t->hovered_close)
                  || (on_new != t->hovered_new);
    t->hovered = i;
    t->hovered_close = on_close;
    t->hovered_new = on_new;
    return changed;
}

static int ops_release(void *w, int cx, int cy) {
    struct uui_tabs *t = (struct uui_tabs *)w;
    int armed = t->pressed, armed_close = t->pressed_close;
    int armed_new = t->pressed_new;
    t->pressed = -1;
    t->pressed_close = 0;
    t->pressed_new = 0;
    if (armed_new) {
        if (hit_new(t, cx, cy) && t->on_new) t->on_new(t->ctx);
        return 1;
    }
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

static void ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_tabs *t = (const struct uui_tabs *)w;
    int x, y, wd, h;
    for (int i = 0; uui_tabs_rect(t, i, &x, &y, &wd, &h); i++)
        uui_describe_rect_i(d, "slot", i, x, y, wd, h);
    if (uui_tabs_new_rect(t, &x, &y, &wd, &h))
        uui_describe_rect(d, "new", x, y, wd, h);
    uui_describe_int(d, "selected", t->selected);
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
    .describe     = ops_describe,
};
