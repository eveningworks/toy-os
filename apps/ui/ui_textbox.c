// See ui_textbox.h for the design writeup.
#include "ui_textbox.h"
#include "ui_primitives.h" // widget_hit(), CURSOR_BAR_W
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
// Hand-rolled shift rather than a memmove() call: this codebase's
// k_memcpy() (string.h) makes no overlapping-region guarantee, and
// TEXTFIELD_MAX is small enough that a plain loop costs nothing.
static void textfield_delete_at_cursor(struct text_field *tf) {
    if (tf->cursor >= tf->len) return;
    for (int i = tf->cursor; i < tf->len - 1; i++) tf->buf[i] = tf->buf[i + 1];
    tf->len--;
    tf->buf[tf->len] = '\0';
}

static void textfield_backspace(struct text_field *tf) {
    if (tf->cursor <= 0) return;
    tf->cursor--;
    textfield_delete_at_cursor(tf);
}

static void textfield_insert(struct text_field *tf, char c) {
    if (tf->len >= TEXTFIELD_MAX - 1) return; // full -- refuse rather than truncate/evict
    for (int i = tf->len; i > tf->cursor; i--) tf->buf[i] = tf->buf[i - 1];
    tf->buf[tf->cursor] = c;
    tf->len++;
    tf->cursor++;
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

    int pad = 4;
    int ty = y + (h - gfx_char_h()) / 2;
    int char_w = gfx_char_w();

    // Clip to what actually fits inside the field, rather than handing
    // gfx_draw_string() the whole buffer -- it has no idea about `w` and
    // just keeps drawing past the border into whatever's next to the
    // field. `visible` is how many characters fit in the padded
    // interior; when the field is active, `start` slides right just far
    // enough to keep the cursor inside that window, so typing past the
    // visible edge scrolls the same way a real text input does.
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
    gfx_draw_string(x + pad, ty, shown, fg, bg);

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

int ui_textbox_hit(const struct ui_textbox *tbx, int cx, int cy) {
    return widget_hit(tbx->x, tbx->y, tbx->w, tbx->h, cx, cy);
}

void ui_textbox_set_active(struct ui_textbox *tbx, int active) {
    widget_textfield_set_active(&tbx->field, active);
}

int ui_textbox_key(struct ui_textbox *tbx, int key) {
    return widget_textfield_key(&tbx->field, key);
}
