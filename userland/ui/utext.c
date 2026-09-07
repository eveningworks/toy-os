// See utext.h for the design: caller-owned flat storage, a wrap
// derived on demand, and a sparse index that only accelerates it.
#include "ui/utext.h"

#define CURSOR_BAR_W 2

char utext_at(const struct utext *t, int i) {
    if (i < 0 || i >= t->count) return 0;
    return t->buf[i];
}

void utext_set(struct utext *t, int i, char c) {
    if (i < 0 || i >= t->count) return;
    t->buf[i] = c;
    t->rev++;
}

void utext_init_buf(struct utext *t, char *buf, int cap) {
    t->buf = buf;
    t->cap = buf ? cap : 0;
    t->count = 0;
    t->rev = 0;
    t->scroll_offset = 0;
    t->ed.cursor = 0;
    t->ed.sel_anchor = 0;
    t->ed.sel_active = 0;
    t->wrap.valid = 0;
}

void utext_clear(struct utext *t) {
    t->count = 0;
    t->rev++;
    t->scroll_offset = 0;
    t->ed.cursor = 0;
    t->ed.sel_anchor = 0;
    t->ed.sel_active = 0;
}

int utext_putc(struct utext *t, char c) {
    if (t->count >= t->cap) return 0;   // refuses; see utext.h
    t->buf[t->count++] = c;
    t->ed.cursor = t->count;
    t->rev++;
    return 1;
}

// --- the ONE wrap accounting ------------------------------------------
//
// Every pass over the document -- building the index, finding a line,
// drawing, hit-testing -- steps through this and nothing else. Two
// copies of it is how draw() and index_at_point() end up one character
// apart on a wrapped line.

struct wrapst { int line, col; };

// Places character `c` (at index `i`) and advances `st`. Returns the
// index at which a NEW line begins because of it, or -1 for neither: a
// '\n' is not drawn and its successor starts the next line, while a
// wrap happens BEFORE `c` is placed, so `c` is the new line's first
// character.
static int wrap_step(struct wrapst *st, char c, int i, int cols) {
    if (c == '\n') { st->line++; st->col = 0; return i + 1; }
    if (st->col >= cols) { st->line++; st->col = 1; return i; }
    st->col++;
    return -1;
}

static void wrap_build(struct utext *t, int cols) {
    struct utext_wrap *w = &t->wrap;
    w->cols = cols;
    w->rev = t->rev;
    w->stride = 1;
    w->n = 0;
    w->idx[w->n++] = 0; // line 0 begins at 0, whatever the text is

    struct wrapst st = { 0, 0 };
    for (int i = 0; i < t->count; i++) {
        int start = wrap_step(&st, t->buf[i], i, cols);
        if (start < 0 || st.line % w->stride) continue;
        if (w->n == UTEXT_CKPTS) {
            // Full: keep every second entry and double the stride, so
            // idx[k] still names line k * stride. One pass, no second
            // scan to decide the stride up front.
            for (int k = 0; k * 2 < w->n; k++) w->idx[k] = w->idx[k * 2];
            w->n = (w->n + 1) / 2;
            w->stride *= 2;
            if (st.line % w->stride) continue;
        }
        w->idx[w->n++] = start;
    }
    w->total_lines = st.line + 1;
    w->valid = 1;
}

static void wrap_ensure(struct utext *t, int cols) {
    struct utext_wrap *w = &t->wrap;
    if (w->valid && w->cols == cols && w->rev == t->rev) return;
    wrap_build(t, cols);
}

// The character index at which wrapped line `line` begins. Scans from
// the nearest checkpoint at or before it -- at most `stride` lines.
static int line_begin(struct utext *t, int cols, int line) {
    struct utext_wrap *w = &t->wrap;
    if (line <= 0) return 0;
    int k = line / w->stride;
    if (k >= w->n) k = w->n - 1;
    struct wrapst st = { k * w->stride, 0 };
    if (st.line >= line) return w->idx[k];
    for (int i = w->idx[k]; i < t->count; i++) {
        int start = wrap_step(&st, t->buf[i], i, cols);
        if (start >= 0 && st.line >= line) return start;
    }
    return t->count;
}

// Where character index `target` is drawn -- and therefore where a
// caret sitting at it belongs, since a caret marks the cell the next
// character will occupy.
//
// **THE PENDING WRAP IS APPLIED BEFORE ANSWERING.** Stepping the
// characters before `target` can leave `col` sitting AT the wrap
// column, a position no character is ever drawn at; draw() resolves
// that when it places the next character, so this has to resolve it
// too or the caret sits one column past the right margin while the
// character it precedes is on the following row.
static void pos_of_index(struct utext *t, int cols, int target,
                          int *out_line, int *out_col) {
    struct utext_wrap *w = &t->wrap;
    int lo = 0, hi = w->n - 1, k = 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (w->idx[m] <= target) { k = m; lo = m + 1; } else hi = m - 1;
    }
    struct wrapst st = { k * w->stride, 0 };
    for (int i = w->idx[k]; i < t->count && i < target; i++)
        wrap_step(&st, t->buf[i], i, cols);
    if (st.col >= cols) { st.line++; st.col = 0; }
    *out_line = st.line;
    *out_col = st.col;
}

static void grid(int w, int h, int *max_cols, int *visible_rows) {
    int cw = ugfx_char_w(), chh = ugfx_char_h();
    *max_cols = cw > 0 ? w / cw : 1;
    if (*max_cols < 1) *max_cols = 1;
    *visible_rows = chh > 0 ? h / chh : 1;
    if (*visible_rows < 1) *visible_rows = 1;
}

// Clamps the scroll and answers the two numbers a scrollbar needs.
static int clamp_scroll(struct utext *t, int total_lines, int visible_rows) {
    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;
    if (t->scroll_offset > max_scroll) t->scroll_offset = max_scroll;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    int first = total_lines - visible_rows - t->scroll_offset;
    return first < 0 ? 0 : first;
}

void utext_scroll(struct utext *t, int delta_lines) {
    t->scroll_offset += delta_lines;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    // The upper clamp needs a width, so it happens in the next measure.
}

// A number no document can exceed: one line per character is the worst
// case, so this cannot fall short and is clamped on the next measure.
void utext_scroll_top(struct utext *t)    { t->scroll_offset = t->count + 1; }
void utext_scroll_bottom(struct utext *t) { t->scroll_offset = 0; }

void utext_metrics(struct utext *t, int w, int h,
                    int *out_total_lines, int *out_visible_rows) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    clamp_scroll(t, t->wrap.total_lines, visible_rows);
    if (out_total_lines) *out_total_lines = t->wrap.total_lines;
    if (out_visible_rows) *out_visible_rows = visible_rows;
}

void utext_reveal_cursor(struct utext *t, int w, int h) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    int total = t->wrap.total_lines;
    int first = clamp_scroll(t, total, visible_rows);

    int cl, cc;
    pos_of_index(t, max_cols, t->ed.cursor, &cl, &cc);
    int want = first;
    if (cl < first) want = cl;
    else if (cl >= first + visible_rows) want = cl - visible_rows + 1;
    if (want == first) return;

    t->scroll_offset = total - visible_rows - want;
    clamp_scroll(t, total, visible_rows);
}

void utext_draw(struct utext *t, struct ugfx_surface *s,
                 int x, int y, int w, int h,
                 uint32_t fg, uint32_t bg, uint32_t sel_bg, int show_cursor) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    int char_w = ugfx_char_w(), char_h = ugfx_char_h();

    ugfx_fill_rect(s, x, y, w, h, bg);

    wrap_ensure(t, max_cols);
    int first_line = clamp_scroll(t, t->wrap.total_lines, visible_rows);

    int sel_start = 0, sel_end = 0;
    int has_sel = utext_sel_present(t);
    if (has_sel) utext_sel_range(t, &sel_start, &sel_end);

    // From the first VISIBLE character, not from index 0: this loop is
    // the reason a big document costs a screenful of work per frame
    // instead of a documentful.
    int line = first_line, col = 0;
    for (int i = line_begin(t, max_cols, first_line); i < t->count; i++) {
        char c = t->buf[i];
        int selected = has_sel && i >= sel_start && i < sel_end;

        if (c == '\n') {
            // A newline is not a glyph, but a selection running through
            // it should still read as reaching the end of the line --
            // so paint a cell's worth of highlight past the last real
            // character rather than stopping short.
            if (selected)
                ugfx_fill_rect(s, x + col * char_w, y + (line - first_line) * char_h,
                                char_w, char_h, sel_bg);
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

        int rx = x + col * char_w;
        int ry = y + (line - first_line) * char_h;
        if (selected) ugfx_fill_rect(s, rx, ry, char_w, char_h, sel_bg);
        ugfx_draw_char(s, rx, ry, c, fg, selected ? sel_bg : bg);
        col++;
    }

    if (show_cursor) {
        int cl, cc;
        pos_of_index(t, max_cols, t->ed.cursor, &cl, &cc);
        if (cl >= first_line && cl - first_line < visible_rows)
            ugfx_fill_rect(s, x + cc * char_w, y + (cl - first_line) * char_h,
                            CURSOR_BAR_W, char_h, fg);
    }
}

int utext_index_at_point(struct utext *t, int x, int y, int w, int h, int px, int py) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    int char_w = ugfx_char_w(), char_h = ugfx_char_h();

    wrap_ensure(t, max_cols);
    int first_line = clamp_scroll(t, t->wrap.total_lines, visible_rows);

    int want_row = (py - y) / char_h;
    if (want_row < 0) want_row = 0;
    int want_line = first_line + want_row;
    int want_col = (px - x) / char_w;
    if (want_col < 0) want_col = 0;

    // The SAME accounting draw() uses, from the same starting point --
    // and IN THE SAME ORDER. The target is tested only after a pending
    // wrap has been applied, because that is when draw() decides which
    // row a character is on; testing first put the caret one character
    // late on every wrapped continuation line, and only there.
    //
    // Anything past the end of the wanted line lands at that line's
    // end, which is what makes clicking in the blank space after a
    // short line behave.
    int line = first_line, col = 0;
    for (int i = line_begin(t, max_cols, first_line); i < t->count; i++) {
        char c = t->buf[i];
        if (c == '\n') {
            if (line == want_line) return i; // clicked past this line's text
            line++;
            col = 0;
            continue;
        }
        if (col >= max_cols) { line++; col = 0; }
        if (line > want_line) return i;      // the wanted line ended first
        if (line == want_line && col >= want_col) return i;
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

static int raw_insert(struct utext *t, int at, char c) {
    if (t->count >= t->cap) return 0;
    for (int i = t->count; i > at; i--) t->buf[i] = t->buf[i - 1];
    t->buf[at] = c;
    t->count++;
    t->rev++;
    return 1;
}

static void raw_erase(struct utext *t, int start, int end) {
    if (start < 0) start = 0;
    if (end > t->count) end = t->count;
    if (start >= end) return;
    int n = end - start;
    for (int i = start; i < t->count - n; i++) t->buf[i] = t->buf[i + n];
    t->count -= n;
    t->rev++;
}

void utext_insert(struct utext *t, char c) {
    if (raw_insert(t, t->ed.cursor, c)) t->ed.cursor++;
}

void utext_delete(struct utext *t) { raw_erase(t, t->ed.cursor, t->ed.cursor + 1); }

void utext_backspace(struct utext *t) {
    if (t->ed.cursor <= 0) return;
    t->ed.cursor--;
    utext_delete(t);
}

// --- the shared edit core's view of this buffer ------------------------

static int ed_len(void *text) { return ((struct utext *)text)->count; }
static char ed_at(void *text, int i) { return utext_at((struct utext *)text, i); }
static int ed_insert(void *text, int i, char c) { return raw_insert((struct utext *)text, i, c); }
static void ed_erase(void *text, int s, int e) { raw_erase((struct utext *)text, s, e); }

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

void utext_sel_all(struct utext *t) {
    t->ed.sel_anchor = 0;
    t->ed.sel_active = 1;
    t->ed.cursor = t->count;
}

int utext_sel_text(const struct utext *t, char *out, int cap) {
    int s, e;
    if (!utext_sel_present(t)) { if (cap > 0) out[0] = '\0'; return 0; }
    utext_sel_range(t, &s, &e);
    int n = e - s;
    for (int i = 0; i < n && i < cap; i++) out[i] = t->buf[s + i];
    if (cap > n) out[n] = '\0';
    return n; // the TRUE length, so an oversized selection can be refused
}

int utext_insert_text(struct utext *t, const char *s, int n) {
    utext_sel_delete(t);
    int put = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\r') continue; // CRLF arrives as this buffer's own '\n'
        if (!raw_insert(t, t->ed.cursor, s[i])) break;
        t->ed.cursor++;
        put++;
    }
    return put;
}

int utext_key(struct utext *t, int key, unsigned mods) {
    return uui_edit_key(&t->ed, &UTEXT_EDIT_OPS, t, key, mods);
}
