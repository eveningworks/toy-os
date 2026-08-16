// See utext.h for the design and what it deliberately keeps from
// apps/ui/ui_scrollback.c.
#include "ui/utext.h"

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
    t->ed.cursor = 0;
    t->ed.sel_anchor = 0;
    t->ed.sel_active = 0;
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
    t->ed.cursor = t->count;
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
        if (i == t->ed.cursor && !captured) {
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

void utext_cursor_left(struct utext *t)  { if (t->ed.cursor > 0) t->ed.cursor--; }
void utext_cursor_right(struct utext *t) { if (t->ed.cursor < t->count) t->ed.cursor++; }
void utext_cursor_home(struct utext *t)  { t->ed.cursor = line_start(t, t->ed.cursor); }
void utext_cursor_end(struct utext *t)   { t->ed.cursor = line_end(t, t->ed.cursor); }

void utext_cursor_up(struct utext *t) {
    int start = line_start(t, t->ed.cursor);
    if (start == 0) return; // already on the first line
    int col = t->ed.cursor - start;
    int prev_start = line_start(t, start - 1);
    int prev_len = (start - 1) - prev_start;
    t->ed.cursor = prev_start + (col < prev_len ? col : prev_len);
}

void utext_cursor_down(struct utext *t) {
    int start = line_start(t, t->ed.cursor);
    int end = line_end(t, t->ed.cursor);
    if (end >= t->count) return; // already on the last line
    int col = t->ed.cursor - start;
    int next_start = end + 1;
    int next_end = line_end(t, next_start);
    int next_len = next_end - next_start;
    t->ed.cursor = next_start + (col < next_len ? col : next_len);
}

// --- editing -----------------------------------------------------------

void utext_insert(struct utext *t, char c) {
    if (t->count >= UTEXT_CAP) return; // refuses; see utext.h
    for (int i = t->count; i > t->ed.cursor; i--) set_at(t, i, utext_at(t, i - 1));
    set_at(t, t->ed.cursor, c);
    t->count++;
    t->ed.cursor++;
}

void utext_delete(struct utext *t) {
    if (t->ed.cursor >= t->count) return;
    for (int i = t->ed.cursor; i < t->count - 1; i++) set_at(t, i, utext_at(t, i + 1));
    t->count--;
}

void utext_backspace(struct utext *t) {
    if (t->ed.cursor <= 0) return;
    t->ed.cursor--;
    utext_delete(t);
}

// --- the shared edit core's accessors ----------------------------------

static int ed_len(void *text) { return ((struct utext *)text)->count; }

static char ed_at(void *text, int i) { return utext_at((struct utext *)text, i); }

static int ed_insert(void *text, int i, char c) {
    struct utext *t = (struct utext *)text;
    if (t->count >= UTEXT_CAP) return 0; // refuses; see utext.h
    for (int k = t->count; k > i; k--) set_at(t, k, utext_at(t, k - 1));
    set_at(t, i, c);
    t->count++;
    return 1;
}

static void ed_erase(void *text, int start, int end) {
    struct utext *t = (struct utext *)text;
    if (start < 0) start = 0;
    if (end > t->count) end = t->count;
    if (start >= end) return;
    int n = end - start;
    for (int i = start; i < t->count - n; i++) set_at(t, i, utext_at(t, i + n));
    t->count -= n;
}

// The four that make this MULTI-LINE. A single-line field leaves these
// NULL and the core declines Up/Down instead of pretending.
static int ed_line_start(void *text, int i) { return line_start((struct utext *)text, i); }
static int ed_line_end(void *text, int i)   { return line_end((struct utext *)text, i); }

static int ed_line_up(void *text, int pos) {
    struct utext *t = (struct utext *)text;
    int start = line_start(t, pos);
    if (start == 0) return pos;               // already on the first line
    int col = pos - start;
    int prev_start = line_start(t, start - 1);
    int prev_len = (start - 1) - prev_start;
    return prev_start + (col < prev_len ? col : prev_len);
}

static int ed_line_down(void *text, int pos) {
    struct utext *t = (struct utext *)text;
    int start = line_start(t, pos);
    int end = line_end(t, pos);
    if (end >= t->count) return pos;          // already on the last line
    int col = pos - start;
    int next_start = end + 1;
    int next_end = line_end(t, next_start);
    int next_len = next_end - next_start;
    return next_start + (col < next_len ? col : next_len);
}

static const struct uui_edit_ops UTEXT_EDIT_OPS = {
    .len = ed_len,
    .at = ed_at,
    .insert = ed_insert,
    .erase = ed_erase,
    .line_start = ed_line_start,
    .line_end = ed_line_end,
    .line_up = ed_line_up,
    .line_down = ed_line_down,
};

// --- selection ---------------------------------------------------------

// These four are the edit core's, delegated rather than reimplemented.
// They keep their old names and behaviour because Notepad calls them
// directly, and a rename would have been churn with no gain.
void utext_sel_start(struct utext *t) {
    t->ed.sel_anchor = t->ed.cursor;
    t->ed.sel_active = 1;
}

void utext_sel_clear(struct utext *t) { uui_edit_clear_selection(&t->ed); }

int utext_sel_present(const struct utext *t) { return uui_edit_has_selection(&t->ed); }

void utext_sel_range(const struct utext *t, int *out_start, int *out_end) {
    uui_edit_range(&t->ed, out_start, out_end);
}

void utext_sel_delete(struct utext *t) {
    uui_edit_delete_selection(&t->ed, &UTEXT_EDIT_OPS, t);
}

// --- the edit core's view of this buffer -------------------------------
//
// The ring is not contiguous, so the core never touches it directly:
// these five (plus the four line ops) are the whole interface, and they
// are the reason a 48-byte field and an 8 KB document can share one
// keymap without sharing storage.
int utext_key(struct utext *t, int key, unsigned mods) {
    return uui_edit_key(&t->ed, &UTEXT_EDIT_OPS, t, key, mods);
}
