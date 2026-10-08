// See ui/uui_actionlist.h.
#include "ui/uui_actionlist.h"
#include "ui/uui_primitives.h"
#include "ui/uui_button.h"
#include "ui/utheme.h"

static int pad(void)    { return ugfx_char_w(); }
static int line_h(void) { return ugfx_char_h() + 2; }
static int row_h(void)  { return 2 * line_h() + ugfx_char_h() / 2 + 2; }

void uui_actionlist_init(struct uui_actionlist *l, struct uui_actionlist_row *rows, int count,
                         const char *empty) {
    *l = (struct uui_actionlist){ .rows = rows, .count = count, .empty = empty,
                                  .hover_row = -1, .hover_btn = -1, .press_row = -1,
                                  .press_btn = -1, .fired_row = -1, .fired_btn = -1 };
}

static int btn_w(const char *label) { return ugfx_text_width(label) + 2 * ugfx_char_w(); }

int uui_actionlist_button_rect(const struct uui_actionlist *l, int row, int btn,
                               int *x, int *y, int *w, int *h) {
    if (row < 0 || row >= l->count || btn < 0 || btn >= UUI_ACTIONLIST_BTNS) return 0;
    const struct uui_actionlist_row *r = &l->rows[row];
    if (!r->btn[btn]) return 0;
    // Right-aligned, the last button outermost: "View only  Disconnect".
    int right = l->x + l->w - pad();
    for (int b = UUI_ACTIONLIST_BTNS - 1; b >= 0; b--) {
        if (!r->btn[b]) continue;
        int bw = btn_w(r->btn[b]);
        if (b == btn) {
            *w = bw;
            *h = utheme_control_h();
            *x = right - bw;
            *y = l->y + row * row_h() + (row_h() - *h) / 2;
            return 1;
        }
        right -= bw + pad() / 2;
    }
    return 0;
}

static int text_w(const struct uui_actionlist *l, int row) {
    int left = l->x + l->w - pad();
    for (int b = 0; b < UUI_ACTIONLIST_BTNS; b++) {
        int x, y, w, h;
        if (uui_actionlist_button_rect(l, row, b, &x, &y, &w, &h) && x < left) left = x;
    }
    return left - pad() - (l->x + pad());
}

void uui_actionlist_draw(struct ugfx_surface *s, const struct uui_actionlist *l, uint32_t bg) {
    uint32_t fg = UTHEME_TEXT, dim = uui_state_bg(fg, UUI_STATE_DISABLED);
    if (!l->count) {
        if (l->empty)
            ugfx_draw_string_clipped(s, l->x + pad(), l->y + (row_h() - ugfx_char_h()) / 2,
                                     l->w - 2 * pad(), l->empty, dim, bg);
        return;
    }
    for (int i = 0; i < l->count; i++) {
        const struct uui_actionlist_row *r = &l->rows[i];
        int ry = l->y + i * row_h();
        if (i) ugfx_fill_rect(s, l->x + pad(), ry, l->w - 2 * pad(), 1, UTHEME_SEPARATOR);
        int tw = text_w(l, i), tx = l->x + pad();
        int ty = ry + (row_h() - 2 * line_h()) / 2;
        if (tw > 0) {
            ugfx_draw_string_elided(s, tx, ty, tw, r->title, fg, bg);
            ugfx_draw_string_elided(s, tx, ty + line_h(), tw, r->sub, dim, bg);
        }
        for (int b = 0; b < UUI_ACTIONLIST_BTNS; b++) {
            int bx, by, bw, bh;
            if (!uui_actionlist_button_rect(l, i, b, &bx, &by, &bw, &bh)) continue;
            int primary = r->primary == b;
            enum uui_state st = l->press_row == i && l->press_btn == b ? UUI_STATE_PRESSED
                             : l->hover_row == i && l->hover_btn == b ? UUI_STATE_HOVER
                             : UUI_STATE_REST;
            uui_button_draw(s, bx, by, bw, bh, r->btn[b], primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG,
                            primary ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, st);
        }
    }
}

static int find(const struct uui_actionlist *l, int cx, int cy, int *row, int *btn) {
    for (int i = 0; i < l->count; i++)
        for (int b = 0; b < UUI_ACTIONLIST_BTNS; b++) {
            int x, y, w, h;
            if (uui_actionlist_button_rect(l, i, b, &x, &y, &w, &h) && uui_hit(x, y, w, h, cx, cy)) {
                *row = i;
                *btn = b;
                return 1;
            }
        }
    *row = *btn = -1;
    return 0;
}

int uui_actionlist_hover(struct uui_actionlist *l, int cx, int cy) {
    int r, b;
    find(l, cx, cy, &r, &b);
    if (r == l->hover_row && b == l->hover_btn) return 0;
    l->hover_row = r;
    l->hover_btn = b;
    return 1;
}

int uui_actionlist_press(struct uui_actionlist *l, int cx, int cy) {
    int r, b;
    if (!find(l, cx, cy, &r, &b)) return 0;
    l->press_row = r;
    l->press_btn = b;
    return 1;
}

int uui_actionlist_release(struct uui_actionlist *l, int cx, int cy) {
    int r, b;
    int was_r = l->press_row, was_b = l->press_btn;
    l->press_row = l->press_btn = -1;
    if (was_r < 0) return 0;
    // Fires only over the button that was pressed, and only if its row is
    // still there: the caller may have rewritten the list meanwhile.
    if (!find(l, cx, cy, &r, &b) || r != was_r || b != was_b || r >= l->count) return 1;
    l->fired_row = r;
    l->fired_btn = b;
    return 2;
}

// --- the ops ---------------------------------------------------------------

static void natural(const void *w, int *ow, int *oh) {
    const struct uui_actionlist *l = w;
    *ow = 0;
    *oh = (l->count ? l->count : 1) * row_h();
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_actionlist *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_actionlist *l = w;
    *x = l->x; *y = l->y; *ow = l->w; *oh = l->h;
}

// Through the ops, on a card of its own -- the setting rows' look, so a
// list sits among them as one more card. A caller drawing it directly
// (the tray flyout) gets the bare rows on its own ground.
static void draw(struct ugfx_surface *s, const void *w) {
    const struct uui_actionlist *l = w;
    uui_fill_round_rect(s, l->x, l->y, l->w, l->h, 6, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, l->x + 1, l->y + 1, l->w - 2, l->h - 2, 5, UTHEME_WHITE);
    uui_actionlist_draw(s, w, UTHEME_WHITE);
}

static int hit(const void *w, int cx, int cy) {
    const struct uui_actionlist *l = w;
    return uui_hit(l->x, l->y, l->w, l->h, cx, cy);
}

static int press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    return uui_actionlist_press(w, cx, cy);
}

static int motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_actionlist *l = w;
    if (buttons) return 0;   // a drag off the button just cancels at release
    return uui_actionlist_hover(l, cx, cy);
}

static int release(void *w, int cx, int cy) { return uui_actionlist_release(w, cx, cy) != 0; }

const struct uui_widget_ops uui_actionlist_ops = {
    .natural_size = natural,
    .set_geometry = geometry,
    .bounds       = bounds,
    .draw         = draw,
    .hit          = hit,
    .press        = press,
    .motion       = motion,
    .release      = release,
};
