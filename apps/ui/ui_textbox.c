// See ui_textbox.h for the design writeup.
#include "ui_focus.h"
#include "ui_textbox.h"
#include "ui_primitives.h" // widget_hit(), CURSOR_BAR_W

// Inset around the text inside the box. Shared by the draw and by
// ui_textbox_natural_size(), so the height reported and the height
// drawn cannot disagree.
#define TEXTBOX_PAD 4
#include "kapi.h"

// ---- struct text_field / widget_textfield_* (the editing implementation) ----

void widget_textfield_init(struct text_field *tf, const char *initial) {
    int i = 0;
    if (initial) {
        while (initial[i] != '\0' && i < TEXTFIELD_MAX - 1) {
            tf->buf[i] = initial[i];
            i++;
        }
    }
    tf->buf[i] = '\0';
    tf->len = i;
    tf->cursor = i;
    tf->active = 0;
}

void widget_textfield_set_active(struct text_field *tf, int active) {
    tf->active = active ? 1 : 0;
}

// Forward-delete the character at the cursor (like a real editor's
// Delete key) -- cursor position doesn't change. No-op at the end.
// This used to be a hand-rolled shift, with a comment explaining that
// k_memcpy() makes no overlapping-region guarantee and there was
// nothing else to call. k_memmove() (string.h) is that something else.
static void textfield_delete_at_cursor(struct text_field *tf) {
    if (tf->cursor >= tf->len) return;
    k_memmove(tf->buf + tf->cursor, tf->buf + tf->cursor + 1,
               (size_t)(tf->len - 1 - tf->cursor));
    tf->len--;
    tf->buf[tf->len] = '\0';
}

static void textfield_backspace(struct text_field *tf) {
    if (tf->cursor <= 0) return;
    tf->cursor = gfx_text_prev(tf->buf, tf->cursor);
    textfield_delete_at_cursor(tf);
}

static void textfield_insert(struct text_field *tf, char c) {
    if (tf->len >= TEXTFIELD_MAX - 1) return; // full -- refuse rather than truncate/evict
    // Overlapping shift right to open a gap -- k_memmove, not k_memcpy
    // (string.h): the regions overlap by everything but one byte.
    k_memmove(tf->buf + tf->cursor + 1, tf->buf + tf->cursor,
               (size_t)(tf->len - tf->cursor));
    tf->buf[tf->cursor] = c;
    tf->len++;
    tf->cursor = gfx_text_next(tf->buf, tf->cursor);
    tf->buf[tf->len] = '\0';
}

int widget_textfield_key(struct text_field *tf, int key) {
    if (!tf->active) return 0;

    if (key == '\b') {
        textfield_backspace(tf);
    } else if (key == KEY_DELETE) {
        textfield_delete_at_cursor(tf);
    } else if (key == KEY_ARROW_LEFT) {
        if (tf->cursor > 0) tf->cursor--;
    } else if (key == KEY_ARROW_RIGHT) {
        if (tf->cursor < tf->len) tf->cursor++;
    } else if (key == KEY_HOME) {
        tf->cursor = 0;
    } else if (key == KEY_END) {
        tf->cursor = tf->len;
    } else if (IS_PRINTABLE_KEY(key)) {
        textfield_insert(tf, (char)key);
    } else {
        return 0; // notably '\r'/'\n' and everything else -- see this fn's doc comment
    }
    return 1;
}

void widget_textfield_draw(int x, int y, int w, int h, const struct text_field *tf,
                            uint32_t bg, uint32_t fg, uint32_t border) {
    gfx_fill_rect(x, y, w, h, bg);
    gfx_draw_rect(x, y, w, h, border);

    int pad = TEXTBOX_PAD;
    int ty = y + (h - gfx_char_h()) / 2;
    int char_w = gfx_char_w();

    // Clip to what actually fits inside the field, rather than handing
    // gfx_draw_string() the whole buffer -- it has no idea about `w` and
    // just keeps drawing past the border into whatever's next to the
    // field. `visible` is how many characters fit in the padded
    // interior; when the field is active, `start` slides right just far
    // enough to keep the cursor inside that window, so typing past the
    // visible edge scrolls the same way a real text input does.
    // Field CAPACITY, which is content-independent -- deliberately not
    // gfx_text_fit_chars(), which measures a particular string. The two
    // coincide only while every glyph is one cell wide; when Milestone
    // 21 makes them differ, this windowing needs real rework, not a
    // helper swap.
    int visible = (w - 2 * pad) / char_w;
    if (visible < 0) visible = 0;
    int start = 0;
    if (tf->active && tf->len > visible) {
        start = tf->cursor - visible + 1;
        if (start < 0) start = 0;
        int max_start = tf->len - visible;
        if (start > max_start) start = max_start;
    }

    char shown[TEXTFIELD_MAX];
    int n = 0;
    for (; n < visible && tf->buf[start + n] != '\0'; n++) shown[n] = tf->buf[start + n];
    shown[n] = '\0';
    // Clipped as a safety net, not because the slice above is expected
    // to be wrong: if that windowing ever miscomputes, the text stops
    // at the field's edge instead of drawing through its border, which
    // is the bug docs/decisions.md records this widget having had.
    gfx_draw_string_clipped(x + pad, ty, w - 2 * pad, shown, fg, bg);

    if (tf->active) {
        int caret_x = x + pad + (tf->cursor - start) * char_w;
        gfx_fill_rect(caret_x, ty, CURSOR_BAR_W, gfx_char_h(), fg);
    }
}

// ---- ui_textbox (the retained-object wrapper) ----

void ui_textbox_init(struct ui_textbox *tbx, int x, int y, int w, int h,
                      const char *initial, uint32_t bg, uint32_t fg, uint32_t border) {
    tbx->x = x;
    tbx->y = y;
    tbx->w = w;
    tbx->h = h;
    tbx->bg = bg;
    tbx->fg = fg;
    tbx->border = border;
    widget_textfield_init(&tbx->field, initial);
}

void ui_textbox_set_geometry(struct ui_textbox *tbx, int x, int y, int w, int h) {
    tbx->x = x;
    tbx->y = y;
    tbx->w = w;
    tbx->h = h;
}

void ui_textbox_draw(const struct ui_textbox *tbx, int origin_x, int origin_y) {
    widget_textfield_draw(origin_x + tbx->x, origin_y + tbx->y, tbx->w, tbx->h,
                           &tbx->field, tbx->bg, tbx->fg, tbx->border);
}

void ui_textbox_natural_size(const struct ui_textbox *tbx, int *out_w, int *out_h) {
    (void)tbx;
    if (out_w) *out_w = 0;                              // no preference
    if (out_h) *out_h = gfx_char_h() + 2 * TEXTBOX_PAD; // and this one is real
}

int ui_textbox_hit(const struct ui_textbox *tbx, int cx, int cy) {
    return widget_hit(tbx->x, tbx->y, tbx->w, tbx->h, cx, cy);
}

void ui_textbox_set_active(struct ui_textbox *tbx, int active) {
    widget_textfield_set_active(&tbx->field, active);
}

int ui_textbox_key(struct ui_textbox *tbx, int key) {
    return widget_textfield_key(&tbx->field, key);
}

// ---- focus integration ----------------------------------------------
//
// See ui_focus.h. This is the widget whose focus is genuinely VISIBLE
// without a ring -- the caret only blinks into existence when the field
// is active -- so `set_focused` drives that, and an app no longer has to
// remember to call ui_textbox_set_active() alongside focusing it. That
// pairing was previously the app's job and is exactly the sort of
// two-things-kept-in-sync-by-hand this directory exists to remove.
static int focus_key(void *w, int key, uint8_t mods) {
    (void)mods; // Ctrl/Alt are already folded into `key` (api/keyboard.h)
    return ui_textbox_key((struct ui_textbox *)w, key);
}

static int focus_hit(const void *w, int cx, int cy) {
    return ui_textbox_hit((const struct ui_textbox *)w, cx, cy);
}

static void focus_ring(const void *w, int ox, int oy, uint32_t color) {
    const struct ui_textbox *tbx = (const struct ui_textbox *)w;
    ui_focus_ring_rect(tbx->x, tbx->y, tbx->w, tbx->h, ox, oy, color);
}

static void focus_set(void *w, int focused) {
    ui_textbox_set_active((struct ui_textbox *)w, focused);
}

const struct ui_focus_ops ui_textbox_focus_ops = {
    .key = focus_key,
    .hit = focus_hit,
    .draw_ring = focus_ring,
    .accepts_focus = 0, // a textbox is always focusable; it has no disabled state yet
    .set_focused = focus_set,
};
