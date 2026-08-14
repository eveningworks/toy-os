// See utext.h for the design and what it deliberately keeps from
// apps/ui/ui_scrollback.c.
#include "utext.h"

#define CURSOR_BAR_W 2 // matches the kernel widget's caret width

char utext_at(const struct utext *t, int i) {
    if (i < 0 || i >= t->count) return 0;
    return t->buf[(t->start + i) % UTEXT_CAP];
}

static void set_at(struct utext *t, int i, char c) {
    t->buf[(t->start + i) % UTEXT_CAP] = c;
}

void utext_init(struct utext *t) {
    t->start = 0;
    t->count = 0;
    t->scroll_offset = 0;
    t->cursor = 0;
    t->sel_anchor = 0;
    t->sel_active = 0;
}

void utext_clear(struct utext *t) { utext_init(t); }

void utext_putc(struct utext *t, char c) {
    if (t->count == UTEXT_CAP) {
        // Full: drop the oldest, same as the kernel widget's append
        // path. Only safe because this is the load path, where the
        // cursor is not yet meaningful -- utext_insert() refuses
        // instead, for the reason given in utext.h.
        t->buf[t->start] = c;
        t->start = (t->start + 1) % UTEXT_CAP;
    } else {
        t->buf[(t->start + t->count) % UTEXT_CAP] = c;
        t->count++;
    }
    t->cursor = t->count;
}

void utext_scroll(struct utext *t, int delta_lines) {
    t->scroll_offset += delta_lines;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    // The upper clamp needs a width, so it happens in measure().
}

// THE one wrap accounting. measure/draw/index_at_point all go through
// this, which is what stops them disagreeing about where a line breaks
// -- see utext.h.
static void measure(struct utext *t, int max_cols, int visible_rows,
                     int *out_last_line, int *out_cursor_line, int *out_cursor_col) {
    int line = 0, col = 0;
    int cursor_line = 0, cursor_col = 0;
    int captured = 0;

    for (int i = 0; i <= t->count; i++) {
        if (i == t->cursor && !captured) {
            cursor_line = line;
            cursor_col = col;
            captured = 1;
        }
        if (i == t->count) break;
        char c = utext_at(t, i);
        if (c == '\n') { line++; col = 0; continue; }
        if (col >= max_cols) { line++; col = 0; }
        col++;
    }

    int total_lines = line + 1;
    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;
    if (t->scroll_offset > max_scroll) t->scroll_offset = max_scroll;
    if (t->scroll_offset < 0) t->scroll_offset = 0;

    if (out_last_line) *out_last_line = line;
    if (out_cursor_line) *out_cursor_line = cursor_line;
    if (out_cursor_col) *out_cursor_col = cursor_col;
}

static void grid(int w, int h, int *max_cols, int *visible_rows) {
    int cw = ugfx_char_w(), chh = ugfx_char_h();
    *max_cols = cw > 0 ? w / cw : 1;
    if (*max_cols < 1) *max_cols = 1;
    *visible_rows = chh > 0 ? h / chh : 1;
    if (*visible_rows < 1) *visible_rows = 1;
}

void utext_metrics(struct utext *t, int w, int h,
                    int *out_total_lines, int *out_visible_rows) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    int last_line;
    measure(t, max_cols, visible_rows, &last_line, 0, 0);
    if (out_total_lines) *out_total_lines = last_line + 1;
    if (out_visible_rows) *out_visible_rows = visible_rows;
}

void utext_draw(struct utext *t, struct ugfx_surface *s,
                 int x, int y, int w, int h,
                 uint32_t fg, uint32_t bg, uint32_t sel_bg, int show_cursor) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    int char_w = ugfx_char_w(), char_h = ugfx_char_h();

    ugfx_fill_rect(s, x, y, w, h, bg);

    int last_line, cursor_line, cursor_col;
    measure(t, max_cols, visible_rows, &last_line, &cursor_line, &cursor_col);
    int total_lines = last_line + 1;

    int first_line = total_lines - visible_rows - t->scroll_offset;
    if (first_line < 0) first_line = 0;

    int sel_start = 0, sel_end = 0;
    int has_sel = utext_sel_present(t);
    if (has_sel) utext_sel_range(t, &sel_start, &sel_end);

    int line = 0, col = 0;
    for (int i = 0; i < t->count; i++) {
        char c = utext_at(t, i);
        int selected = has_sel && i >= sel_start && i < sel_end;

        if (c == '\n') {
            // A newline is not a glyph, but a selection running through
            // it should still read as reaching the end of the line --
            // so paint a cell's worth of highlight past the last real
            // character rather than stopping short.
            if (selected && line >= first_line && line - first_line < visible_rows) {
                ugfx_fill_rect(s, x + col * char_w, y + (line - first_line) * char_h,
                                char_w, char_h, sel_bg);
            }
            line++;
            col = 0;
            if (line - first_line >= visible_rows) break;
            continue;
        }

        if (col >= max_cols) {
            line++;
            col = 0;
            if (line - first_line >= visible_rows) break;
        }

        if (line >= first_line) {
            int rx = x + col * char_w;
            int ry = y + (line - first_line) * char_h;
            uint32_t cell_bg = selected ? sel_bg : bg;
            if (selected) ugfx_fill_rect(s, rx, ry, char_w, char_h, sel_bg);
            ugfx_draw_char(s, rx, ry, c, fg, cell_bg);
        }
        col++;
    }

    if (show_cursor && cursor_line >= first_line && cursor_line - first_line < visible_rows) {
        ugfx_fill_rect(s, x + cursor_col * char_w,
                        y + (cursor_line - first_line) * char_h,
                        CURSOR_BAR_W, char_h, fg);
    }
}

int utext_index_at_point(struct utext *t, int x, int y, int w, int h, int px, int py) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    int char_w = ugfx_char_w(), char_h = ugfx_char_h();

    int last_line;
    measure(t, max_cols, visible_rows, &last_line, 0, 0);
    int total_lines = last_line + 1;

    int first_line = total_lines - visible_rows - t->scroll_offset;
    if (first_line < 0) first_line = 0;

    int want_row = (py - y) / char_h;
    if (want_row < 0) want_row = 0;
    int want_line = first_line + want_row;
    int want_col = (px - x) / char_w;
    if (want_col < 0) want_col = 0;

    // Walk with the SAME accounting draw() uses, and stop at the first
    // index whose (line, col) reaches the target. Anything past the end
    // of the wanted line lands at that line's end, which is what makes
    // clicking in the blank space after a short line behave.
    int line = 0, col = 0;
    for (int i = 0; i < t->count; i++) {
        char c = utext_at(t, i);
        if (line == want_line && col >= want_col) return i;
        if (c == '\n') {
            if (line == want_line) return i; // clicked past this line's text
            line++;
            col = 0;
            continue;
        }
        if (col >= max_cols) {
            if (line == want_line) return i; // past the wrap point
            line++;
            col = 0;
        }
        col++;
    }
    return t->count; // below the last line, or past the end
}

// --- cursor movement ---------------------------------------------------

static int line_start(const struct utext *t, int pos) {
    int i = pos;
    while (i > 0 && utext_at(t, i - 1) != '\n') i--;
    return i;
}

static int line_end(const struct utext *t, int pos) {
    int i = pos;
    while (i < t->count && utext_at(t, i) != '\n') i++;
    return i;
}

void utext_cursor_left(struct utext *t)  { if (t->cursor > 0) t->cursor--; }
void utext_cursor_right(struct utext *t) { if (t->cursor < t->count) t->cursor++; }
void utext_cursor_home(struct utext *t)  { t->cursor = line_start(t, t->cursor); }
void utext_cursor_end(struct utext *t)   { t->cursor = line_end(t, t->cursor); }

void utext_cursor_up(struct utext *t) {
    int start = line_start(t, t->cursor);
    if (start == 0) return; // already on the first line
    int col = t->cursor - start;
    int prev_start = line_start(t, start - 1);
    int prev_len = (start - 1) - prev_start;
    t->cursor = prev_start + (col < prev_len ? col : prev_len);
}

void utext_cursor_down(struct utext *t) {
    int start = line_start(t, t->cursor);
    int end = line_end(t, t->cursor);
    if (end >= t->count) return; // already on the last line
    int col = t->cursor - start;
    int next_start = end + 1;
    int next_end = line_end(t, next_start);
    int next_len = next_end - next_start;
    t->cursor = next_start + (col < next_len ? col : next_len);
}

// --- editing -----------------------------------------------------------

void utext_insert(struct utext *t, char c) {
    if (t->count >= UTEXT_CAP) return; // refuses; see utext.h
    for (int i = t->count; i > t->cursor; i--) set_at(t, i, utext_at(t, i - 1));
    set_at(t, t->cursor, c);
    t->count++;
    t->cursor++;
}

void utext_delete(struct utext *t) {
    if (t->cursor >= t->count) return;
    for (int i = t->cursor; i < t->count - 1; i++) set_at(t, i, utext_at(t, i + 1));
    t->count--;
}

void utext_backspace(struct utext *t) {
    if (t->cursor <= 0) return;
    t->cursor--;
    utext_delete(t);
}

// --- selection ---------------------------------------------------------

void utext_sel_start(struct utext *t) {
    t->sel_anchor = t->cursor;
    t->sel_active = 1;
}

void utext_sel_clear(struct utext *t) {
    t->sel_active = 0;
    t->sel_anchor = 0;
}

int utext_sel_present(const struct utext *t) {
    return t->sel_active && t->sel_anchor != t->cursor;
}

void utext_sel_range(const struct utext *t, int *out_start, int *out_end) {
    int a = t->sel_anchor, b = t->cursor;
    if (a > b) { int tmp = a; a = b; b = tmp; }
    if (out_start) *out_start = a;
    if (out_end) *out_end = b;
}

void utext_sel_delete(struct utext *t) {
    if (!utext_sel_present(t)) return;
    int a, b;
    utext_sel_range(t, &a, &b);
    int n = b - a;
    for (int i = a; i < t->count - n; i++) set_at(t, i, utext_at(t, i + n));
    t->count -= n;
    t->cursor = a;
    utext_sel_clear(t);
}
