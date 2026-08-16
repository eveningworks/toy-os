// field. Split out of uwidgets.c -- see ui/uui_textbox.h.
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"  // the ops tables at the bottom of this file
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// Caret width in pixels -- a bar, not a block, so it sits between
// characters rather than covering one.
#define CARET_W 2

// Inset around the text inside the box. Shared by the draw and by
// uui_textbox_natural_size(), so the height reported and the height drawn
// cannot disagree.
#define UUI_TEXTBOX_PAD 4

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------

void uui_textbox_init(struct uui_textbox *f, const char *initial) {
    int i = 0;
    if (initial) {
        while (initial[i] && i < UUI_TEXTBOX_MAX - 1) { f->buf[i] = initial[i]; i++; }
    }
    f->buf[i] = '\0';
    f->len = i;
    f->cursor = i;
    f->active = 0;
}

void uui_textbox_set_active(struct uui_textbox *f, int active) { f->active = active ? 1 : 0; }

static void field_insert(struct uui_textbox *f, char c) {
    if (f->len >= UUI_TEXTBOX_MAX - 1) return;
    for (int i = f->len; i > f->cursor; i--) f->buf[i] = f->buf[i - 1];
    f->buf[f->cursor] = c;
    f->len++;
    f->cursor = ugfx_text_next(f->buf, f->cursor);
    f->buf[f->len] = '\0';
}

static void field_delete(struct uui_textbox *f) {
    if (f->cursor >= f->len) return;
    for (int i = f->cursor; i < f->len - 1; i++) f->buf[i] = f->buf[i + 1];
    f->len--;
    f->buf[f->len] = '\0';
}

void uui_textbox_natural_size(const struct uui_textbox *f, int *out_w, int *out_h) {
    (void)f;
    if (out_w) *out_w = 0;                             // no preference
    if (out_h) *out_h = ugfx_char_h() + 2 * UUI_TEXTBOX_PAD; // and this one is real
}

// How far the visible window has slid right, in characters. Shared by
// draw() and by uui_textbox_index_at_x() below, so a click lands on the
// character that is actually drawn there -- two copies of this
// arithmetic is the classic way a caret ends up one glyph off.
static int field_window_start(const struct uui_textbox *f, int visible) {
    if (!f->active || f->len <= visible) return 0;
    int start = f->cursor - visible + 1;
    if (start < 0) start = 0;
    int max_start = f->len - visible;
    if (start > max_start) start = max_start;
    return start;
}

int uui_textbox_index_at_x(const struct uui_textbox *f, int cx) {
    int char_w = ugfx_char_w();
    if (char_w <= 0) return f->cursor;
    int pad = UUI_TEXTBOX_PAD;
    int visible = (f->w - 2 * pad) / char_w;
    if (visible < 0) visible = 0;

    int rel = cx - (f->x + pad);
    if (rel < 0) rel = 0;
    // Round to the NEAREST gap rather than truncating: clicking the
    // right half of a glyph should put the caret after it, which is
    // what a text field does everywhere else.
    int col = (rel + char_w / 2) / char_w;
    int idx = field_window_start(f, visible) + col;
    if (idx < 0) idx = 0;
    if (idx > f->len) idx = f->len;
    return idx;
}

int uui_textbox_key(struct uui_textbox *f, int key) {
    if (!f->active) return 0;

    if (key == '\b') {
        if (f->cursor > 0) { f->cursor = ugfx_text_prev(f->buf, f->cursor); field_delete(f); }
    } else if (key == KEY_DELETE) {
        field_delete(f);
    } else if (key == KEY_ARROW_LEFT) {
        f->cursor = ugfx_text_prev(f->buf, f->cursor);
    } else if (key == KEY_ARROW_RIGHT) {
        if (f->cursor < f->len) f->cursor = ugfx_text_next(f->buf, f->cursor);
    } else if (key == KEY_HOME) {
        f->cursor = 0;
    } else if (key == KEY_END) {
        f->cursor = f->len;
    } else if (key >= 32 && key < 127) {
        field_insert(f, (char)key);
    } else {
        // Notably Enter: committing a field is the caller's decision.
        return 0;
    }
    return 1;
}

void uui_textbox_set_geometry(struct uui_textbox *f, int x, int y, int w, int h) {
    f->x = x; f->y = y; f->w = w; f->h = h;
}

int uui_textbox_hit(const struct uui_textbox *f, int cx, int cy) {
    return uui_hit(f->x, f->y, f->w, f->h, cx, cy);
}

void uui_textbox_draw(struct ugfx_surface *s, const struct uui_textbox *f,
                     uint32_t bg, uint32_t fg, uint32_t border) {
    int x = f->x, y = f->y, w = f->w, h = f->h;
    ugfx_fill_rect(s, x, y, w, h, bg);
    ugfx_draw_rect(s, x, y, w, h, border);

    int pad = UUI_TEXTBOX_PAD;
    int char_w = ugfx_char_w();
    int ty = y + (h - ugfx_char_h()) / 2;

    // Horizontal windowing: how many characters fit, and how far right
    // the window has to slide to keep the caret inside it. Without this
    // a long value draws straight through the border -- a bug the
    // kernel widget actually had.
    int visible = char_w > 0 ? (w - 2 * pad) / char_w : 0;
    if (visible < 0) visible = 0;
    int start = field_window_start(f, visible);

    char shown[UUI_TEXTBOX_MAX];
    int n = 0;
    for (; n < visible && f->buf[start + n]; n++) shown[n] = f->buf[start + n];
    shown[n] = '\0';
    // Clipped as a safety net, not because the slice is expected to be
    // wrong: if the windowing ever miscomputes, text stops at the edge
    // rather than drawing through the border.
    ugfx_draw_string_clipped(s, x + pad, ty, w - 2 * pad, shown, fg, bg);

    if (f->active) {
        ugfx_fill_rect(s, x + pad + (f->cursor - start) * char_w, ty,
                        CARET_W, ugfx_char_h(), fg);
    }
}

// --- focus ------------------------------------------------------------
//
// A FOCUS-ONLY ops table: hit/key/set_focused/accepts_focus, with the
// drawing and layout slots left NULL. Every slot is optional (see
// uui_widget.h), and a field's draw takes three colours the generic
// signature has nowhere to carry -- so this table is what uui_focus
// needs and nothing more, rather than a half-honest full one.
static int ops_hit(const void *w, int cx, int cy) {
    return uui_textbox_hit((const struct uui_textbox *)w, cx, cy);
}
static int ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_textbox_key((struct uui_textbox *)w, key);
}
static void ops_set_focused(void *w, int focused) {
    uui_textbox_set_active((struct uui_textbox *)w, focused);
}
static int ops_accepts_focus(const void *w) { (void)w; return 1; }

const struct uui_widget_ops uui_textbox_focus_ops = {
    .hit = ops_hit,
    .key = ops_key,
    .set_focused = ops_set_focused,
    .accepts_focus = ops_accepts_focus,
};

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// A press PLACES THE CARET, which is what every text field on every
// desktop does and what this one could not do at all: the caret moved
// only with arrow keys, so clicking into the middle of a value did
// nothing and you had to walk there. The character index comes from the
// same horizontal-window arithmetic draw() uses (see caret_index()), so
// clicking a glyph puts the caret at that glyph rather than near it.
static int tb_ops_press(void *w, int cx, int cy) {
    (void)cy;
    struct uui_textbox *f = (struct uui_textbox *)w;
    f->cursor = uui_textbox_index_at_x(f, cx);
    return 1;
}

const struct uui_widget_ops uui_textbox_ops = {
    .hit           = ops_hit,
    .key           = ops_key,
    .set_focused   = ops_set_focused,
    .accepts_focus = ops_accepts_focus,
    .press         = tb_ops_press,
};
