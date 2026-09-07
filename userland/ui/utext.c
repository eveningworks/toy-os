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
    t->wrap = UTEXT_WRAP_WORD;
    t->hscroll = 0;
    t->scroll_offset = 0;
    t->ed.cursor = 0;
    t->ed.sel_anchor = 0;
    t->ed.sel_active = 0;
    t->wrap_cache.valid = 0;
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
// drawing, hit-testing -- goes through line_span() and nothing else.
// Two copies of it is how draw() and index_at_point() end up one
// character apart on a wrapped line.

// Where the visual line beginning at `start` ends.
//
//   *draw_end  one past the last character DRAWN on it
//   *next      where the NEXT visual line begins
//
// The two differ by whatever the break consumed: a '\n' is not drawn,
// and neither are the spaces a word wrap breaks at -- which is what
// stops a wrapped line beginning with the blanks that ended the last
// one.
static void line_span(const struct utext *t, int cols, int start,
                       int *draw_end, int *next) {
    if (cols < 1) cols = 1;

    // NO WRAP: a line is exactly what the author typed. It may be far
    // wider than the view; that is what hscroll is for.
    if (t->wrap == UTEXT_WRAP_OFF) {
        int i = start;
        while (i < t->count && t->buf[i] != '\n') i++;
        *draw_end = i;
        *next = i < t->count ? i + 1 : i;
        return;
    }

    int col = 0;
    int run = -1;        // first character of the current run of spaces
    int cand_end = -1;   // best break so far: draw to here...
    int cand_next = -1;  // ...and resume there
    for (int i = start; i < t->count; i++) {
        char c = t->buf[i];
        if (c == '\n') { *draw_end = i; *next = i + 1; return; }

        if (c == ' ' || c == '\t') {
            // A SPACE NEVER TRIGGERS A BREAK. Trailing blanks are
            // invisible, so pushing a line over the edge with them
            // would break before a word that fits perfectly well.
            if (run < 0) run = i;
            col++;
            continue;
        }

        // The first character after a run of spaces completes a break
        // candidate. Not one at `start` -- breaking there would make no
        // progress and the caller would loop forever on the same line.
        if (run > start) { cand_end = run; cand_next = i; }
        run = -1;

        if (col >= cols) {
            if (cand_end > start) { *draw_end = cand_end; *next = cand_next; return; }
            // A WORD WIDER THAN THE VIEW. It has nowhere else to go, so
            // it breaks hard -- as it does in every editor, and as this
            // widget did for every word before word wrap existed.
            *draw_end = i;
            *next = i;
            return;
        }
        col++;
    }
    *draw_end = t->count;
    *next = t->count;
}

// **WHERE THE WALK STOPS, STATED ONCE.** A span is the last one when it
// reached the end of the text AND the break consumed nothing -- because
// a break that DID consume something (a '\n', or the spaces a word wrap
// swallowed) leaves a real, empty line after it, which an editor shows
// and a caret can sit on. Getting this wrong counts a phantom line at
// the end of every document, which then shifts the scroll clamp and the
// whole visible window by a row.
static int last_span(const struct utext *t, int draw_end, int next) {
    return next >= t->count && next == draw_end;
}

static void wrap_build(struct utext *t, int cols) {
    struct utext_wrap *w = &t->wrap_cache;
    w->cols = cols;
    w->rev = t->rev;
    w->wrap = t->wrap;
    w->stride = 1;
    w->n = 0;
    w->idx[w->n++] = 0; // line 0 begins at 0, whatever the text is

    int line = 0, i = 0;
    for (;;) {
        int draw_end, next;
        line_span(t, cols, i, &draw_end, &next);
        if (last_span(t, draw_end, next)) break;
        if (next <= i) break;   // line_span never fails to advance; belt and braces
        i = next;
        line++;
        if (line % w->stride) continue;
        if (w->n == UTEXT_CKPTS) {
            // Full: keep every second entry and double the stride, so
            // idx[k] still names line k * stride. One pass, no second
            // scan to decide the stride up front.
            for (int k = 0; k * 2 < w->n; k++) w->idx[k] = w->idx[k * 2];
            w->n = (w->n + 1) / 2;
            w->stride *= 2;
            if (line % w->stride) continue;
        }
        w->idx[w->n++] = i;
    }
    w->total_lines = line + 1;
    w->valid = 1;
}

static void wrap_ensure(struct utext *t, int cols) {
    struct utext_wrap *w = &t->wrap_cache;
    if (w->valid && w->cols == cols && w->rev == t->rev && w->wrap == t->wrap) return;
    wrap_build(t, cols);
}

// The character index at which visual line `line` begins. Scans from
// the nearest checkpoint at or before it -- at most `stride` lines.
static int line_begin(struct utext *t, int cols, int line) {
    struct utext_wrap *w = &t->wrap_cache;
    if (line <= 0) return 0;
    int k = line / w->stride;
    if (k >= w->n) k = w->n - 1;
    int cur = k * w->stride;
    int i = w->idx[k];
    while (cur < line) {
        int draw_end, next;
        line_span(t, cols, i, &draw_end, &next);
        if (last_span(t, draw_end, next) || next <= i) break;
        i = next;
        cur++;
    }
    return i;
}

// Where character index `target` is drawn: its visual line, and its
// column WITHIN that line. A target sitting in the gap a break consumed
// -- a '\n', or the spaces a word wrap swallowed -- reports the end of
// the line it belongs to, which is where a caret there should sit.
static void pos_of_index(struct utext *t, int cols, int target,
                          int *out_line, int *out_col) {
    struct utext_wrap *w = &t->wrap_cache;
    int lo = 0, hi = w->n - 1, k = 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (w->idx[m] <= target) { k = m; lo = m + 1; } else hi = m - 1;
    }
    int line = k * w->stride;
    int i = w->idx[k];
    for (;;) {
        int draw_end, next;
        line_span(t, cols, i, &draw_end, &next);
        if (target <= draw_end || last_span(t, draw_end, next) || next <= i) {
            *out_line = line;
            *out_col = (target < i ? 0 : (target > draw_end ? draw_end : target) - i);
            return;
        }
        i = next;
        line++;
    }
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
    clamp_scroll(t, t->wrap_cache.total_lines, visible_rows);
    if (out_total_lines) *out_total_lines = t->wrap_cache.total_lines;
    if (out_visible_rows) *out_visible_rows = visible_rows;
}

void utext_reveal_cursor(struct utext *t, int w, int h) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    int total = t->wrap_cache.total_lines;
    int first = clamp_scroll(t, total, visible_rows);

    int cl, cc;
    pos_of_index(t, max_cols, t->ed.cursor, &cl, &cc);

    // SIDEWAYS TOO, when there is a sideways. Without this, typing past
    // the right edge of an unwrapped document changes text nobody can
    // see -- the caret is off-screen and the view never follows it.
    if (t->wrap == UTEXT_WRAP_OFF) {
        if (cc < t->hscroll) t->hscroll = cc;
        else if (cc >= t->hscroll + max_cols) t->hscroll = cc - max_cols + 1;
        if (t->hscroll < 0) t->hscroll = 0;
    }

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
    int first_line = clamp_scroll(t, t->wrap_cache.total_lines, visible_rows);

    int sel_start = 0, sel_end = 0;
    int has_sel = utext_sel_present(t);
    if (has_sel) utext_sel_range(t, &sel_start, &sel_end);

    // ROW BY ROW, from the first VISIBLE line -- which is why a
    // document of any size costs a screenful of work per frame rather
    // than a documentful.
    int i = line_begin(t, max_cols, first_line);
    for (int row = 0; row < visible_rows && i <= t->count; row++) {
        int draw_end, next;
        line_span(t, max_cols, i, &draw_end, &next);

        int ry = y + row * char_h;
        for (int k = i; k < draw_end; k++) {
            int col = k - i - t->hscroll;   // hscroll is 0 while wrapping
            if (col < 0) continue;
            if (col >= max_cols) break;
            int selected = has_sel && k >= sel_start && k < sel_end;
            int rx = x + col * char_w;
            if (selected) ugfx_fill_rect(s, rx, ry, char_w, char_h, sel_bg);
            ugfx_draw_char(s, rx, ry, t->buf[k], fg, selected ? sel_bg : bg);
        }
        // A selection running THROUGH a line break should read as
        // reaching the end of the line, so paint one cell past the last
        // character rather than stopping short.
        if (has_sel && draw_end >= sel_start && draw_end < sel_end) {
            int col = draw_end - i - t->hscroll;
            if (col >= 0 && col < max_cols)
                ugfx_fill_rect(s, x + col * char_w, ry, char_w, char_h, sel_bg);
        }

        if (last_span(t, draw_end, next) || next <= i) break;
        i = next;
    }

    if (show_cursor) {
        int cl, cc;
        pos_of_index(t, max_cols, t->ed.cursor, &cl, &cc);
        cc -= t->hscroll;
        if (cl >= first_line && cl - first_line < visible_rows &&
            cc >= 0 && cc <= max_cols)
            ugfx_fill_rect(s, x + cc * char_w, y + (cl - first_line) * char_h,
                            CURSOR_BAR_W, char_h, fg);
    }
}

int utext_index_at_point(struct utext *t, int x, int y, int w, int h, int px, int py) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    int char_w = ugfx_char_w(), char_h = ugfx_char_h();

    wrap_ensure(t, max_cols);
    int first_line = clamp_scroll(t, t->wrap_cache.total_lines, visible_rows);

    int want_row = (py - y) / char_h;
    if (want_row < 0) want_row = 0;
    if (want_row >= visible_rows) want_row = visible_rows - 1;
    int want_col = (px - x) / char_w + t->hscroll;
    if (want_col < 0) want_col = 0;

    // The SAME spans draw() used, walked the same way. Clicking past a
    // line's end lands at that end, which is what makes clicking in the
    // blank space after a short line behave.
    int i = line_begin(t, max_cols, first_line);
    for (int row = 0; row <= want_row; row++) {
        int draw_end, next;
        line_span(t, max_cols, i, &draw_end, &next);
        if (row == want_row) {
            int at = i + want_col;
            return at > draw_end ? draw_end : at;
        }
        if (last_span(t, draw_end, next) || next <= i)
            return draw_end;   // clicked below the last line
        i = next;
    }
    return t->count;
}

int utext_widest_line(struct utext *t, int w, int h) {
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    // While wrapping there is nothing to the right of the view by
    // construction, so the honest answer is the view's own width.
    if (t->wrap != UTEXT_WRAP_OFF) return max_cols;

    int widest = 0;
    for (int i = 0;;) {
        int draw_end, next;
        line_span(t, max_cols, i, &draw_end, &next);
        if (draw_end - i > widest) widest = draw_end - i;
        if (last_span(t, draw_end, next) || next <= i) break;
        i = next;
    }
    return widest;
}

void utext_set_wrap(struct utext *t, int mode) {
    if (t->wrap == mode) return;
    t->wrap = mode;
    // A DOCUMENT COMING BACK SIDEWAYS with no way to say so is what an
    // hscroll left set would look like: wrapping puts nothing to the
    // right of the view, so the scrollbar that moved it is gone too.
    if (mode != UTEXT_WRAP_OFF) t->hscroll = 0;
    t->wrap_cache.valid = 0;
}

int utext_get_wrap(const struct utext *t) { return t->wrap; }

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
