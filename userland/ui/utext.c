// See utext.h for the design: caller-owned flat storage, a wrap
// derived on demand, and a sparse index that only accelerates it.
//
// **THIS BUFFER IS A FIXED GRID, AND THAT IS A CONTRACT ON ITS
// CALLERS.** Every glyph is placed at `col * ugfx_char_w()` and every
// hit-test divides by it, which is a measurement only in a monospace
// face. So a caller must select one around anything here that draws or
// measures -- Notepad's `doc_font()` and `uui_textview.c`'s
// `grid_font()` are the two that do. Drawn in the proportional
// interface face this spreads text at cell pitch with ragged gaps and
// puts the caret nowhere near the click.
#include "ui/utext.h"
#include "ui/uui_caret.h"
#include "ui/uui_undo.h"
#include "ui/uui_widget.h"   // uui_key_is_shortcut

#define CURSOR_BAR_W 2

// **A TAB IS AS WIDE AS THE GAP TO THE NEXT STOP**, counted from the
// start of the visual row. `tab_width` 0 or 1 keeps the old one cell.
// Every pass that turns indices into columns goes through these two --
// drawing and hit-testing disagreeing by a tab is the bug they prevent.
static int cells_at(const struct utext *t, char c, int col) {
    if (c != '\t' || t->tab_width <= 1) return 1;
    return t->tab_width - col % t->tab_width;
}

// The column of index `k` on the visual row that begins at `row`.
static int col_of(const struct utext *t, int row, int k) {
    int col = 0;
    for (int i = row; i < k && i < t->count; i++) col += cells_at(t, t->buf[i], col);
    return col;
}

char utext_at(const struct utext *t, int i) {
    if (i < 0 || i >= t->count) return 0;
    return t->buf[i];
}

// A load is not an edit: what was recorded no longer describes the text.
static void forget_history(struct utext *t) {
    if (t->ed.undo) uui_undo_reset(t->ed.undo);
}

void utext_set(struct utext *t, int i, char c) {
    if (i < 0 || i >= t->count) return;
    t->buf[i] = c;
    t->rev++;
    forget_history(t);
}

void utext_init_buf(struct utext *t, char *buf, int cap) {
    t->buf = buf;
    t->cap = buf ? cap : 0;
    t->count = 0;
    t->rev = 0;
    t->wrap = UTEXT_WRAP_WORD;
    t->hscroll = 0;
    t->scroll_offset = 0;
    t->animate = 0;
    uui_scrollanim_init(&t->anim);
    uui_edit_init(&t->ed);
    t->wrap_cache.valid = 0;
    t->gutter = 0;
    t->line_highlight = 0;
    t->gutter_fg = t->gutter_bg = t->line_bg = 0;
    t->tab_width = t->tab_spaces = t->auto_indent = t->show_ws = t->scroll_margin = 0;
    t->marks = 0;
    t->mark_count = 0;
    t->nl_cache.valid = 0;
}

void utext_clear(struct utext *t) {
    t->count = 0;
    t->rev++;
    t->scroll_offset = 0;
    t->ed.cursor = 0;
    t->ed.sel_anchor = 0;
    t->ed.sel_active = 0;
    forget_history(t);
}

int utext_putc(struct utext *t, char c) {
    if (t->count >= t->cap) return 0;   // refuses; see utext.h
    t->buf[t->count++] = c;
    t->ed.cursor = t->count;
    t->rev++;
    forget_history(t);
    return 1;
}

// --- logical lines, for the gutter and for callers ---------------------

int utext_line_count(struct utext *t) {
    if (t->nl_cache.valid && t->nl_cache.rev == t->rev) return t->nl_cache.lines;
    int n = 1;
    for (int i = 0; i < t->count; i++) if (t->buf[i] == '\n') n++;
    t->nl_cache.valid = 1;
    t->nl_cache.rev = t->rev;
    t->nl_cache.lines = n;
    return n;
}

void utext_line_col(const struct utext *t, int i, int *line, int *col) {
    if (i > t->count) i = t->count;
    int ln = 0, start = 0;
    for (int k = 0; k < i; k++) if (t->buf[k] == '\n') { ln++; start = k + 1; }
    if (line) *line = ln;
    if (col) *col = i - start;
}

int utext_line_index(const struct utext *t, int line) {
    if (line <= 0) return 0;
    for (int k = 0; k < t->count; k++)
        if (t->buf[k] == '\n' && --line == 0) return k + 1;
    return t->count;
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
            col += cells_at(t, c, col);
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
    w->lno[w->n] = 0;
    w->idx[w->n++] = 0; // line 0 begins at 0, whatever the text is

    int line = 0, i = 0, logical = 0;
    for (;;) {
        int draw_end, next;
        line_span(t, cols, i, &draw_end, &next);
        if (last_span(t, draw_end, next)) break;
        if (next <= i) break;   // line_span never fails to advance; belt and braces
        if (t->buf[next - 1] == '\n') logical++;
        i = next;
        line++;
        if (line % w->stride) continue;
        if (w->n == UTEXT_CKPTS) {
            // Full: keep every second entry and double the stride, so
            // idx[k] still names line k * stride. One pass, no second
            // scan to decide the stride up front.
            for (int k = 0; k * 2 < w->n; k++) {
                w->idx[k] = w->idx[k * 2];
                w->lno[k] = w->lno[k * 2];
            }
            w->n = (w->n + 1) / 2;
            w->stride *= 2;
            if (line % w->stride) continue;
        }
        w->lno[w->n] = logical;
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

// The character index at which visual line `line` begins, and the
// logical line it is part of. Scans from the nearest checkpoint at or
// before it -- at most `stride` lines.
static int line_begin_ex(struct utext *t, int cols, int line, int *logical) {
    struct utext_wrap *w = &t->wrap_cache;
    if (line <= 0) { if (logical) *logical = 0; return 0; }
    int k = line / w->stride;
    if (k >= w->n) k = w->n - 1;
    int cur = k * w->stride;
    int i = w->idx[k];
    int lg = w->lno[k];
    while (cur < line) {
        int draw_end, next;
        line_span(t, cols, i, &draw_end, &next);
        if (last_span(t, draw_end, next) || next <= i) break;
        if (t->buf[next - 1] == '\n') lg++;
        i = next;
        cur++;
    }
    if (logical) *logical = lg;
    return i;
}

static int line_begin(struct utext *t, int cols, int line) {
    return line_begin_ex(t, cols, line, 0);
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
            *out_col = target < i ? 0 : col_of(t, i, target > draw_end ? draw_end : target);
            return;
        }
        i = next;
        line++;
    }
}

static void grid(int w, int h, int *max_cols, int *visible_rows) {
    int cw = ugfx_char_w(), chh = ugfx_char_h();
    // text-measure-ok: the caller selected a mono face; see the top of this file
    *max_cols = cw > 0 ? w / cw : 1;
    if (*max_cols < 1) *max_cols = 1;
    *visible_rows = chh > 0 ? h / chh : 1;
    if (*visible_rows < 1) *visible_rows = 1;
}

// THE GUTTER'S WIDTH: the widest line number (three digits at least,
// so it does not jump at line 10 and 100) plus a cell either side.
static int gutter_digits(struct utext *t) {
    int n = utext_line_count(t), d = 1;
    while (n >= 10) { n /= 10; d++; }
    return d < 3 ? 3 : d;
}

static int gutter_px(struct utext *t) {
    if (!t->gutter) return 0;
    // text-measure-ok: the caller selected a mono face; see the top of this file
    return (gutter_digits(t) + 2) * ugfx_char_w();
}

// Every box a caller passes is the gutter AND the text; this is the
// text's share of it, stated once so draw and hit-test cannot disagree.
static void text_box(struct utext *t, int *x, int *w) {
    int g = gutter_px(t);
    if (g <= 0) return;
    if (g > *w - ugfx_char_w()) g = *w - ugfx_char_w();
    if (g < 0) g = 0;
    *x += g;
    *w -= g;
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
    if (t->animate) uui_scrollanim_arm(&t->anim);
    t->scroll_offset += delta_lines;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    // The upper clamp needs a width, so it happens in the next measure.
}

// A number no document can exceed: one line per character is the worst
// case, so this cannot fall short and is clamped on the next measure.
void utext_scroll_top(struct utext *t)    { utext_scroll_set(t, t->count + 1); }
void utext_scroll_bottom(struct utext *t) { utext_scroll_set(t, 0); }

void utext_scroll_set(struct utext *t, int offset) {
    uui_scrollanim_cancel(&t->anim);
    t->scroll_offset = offset < 0 ? 0 : offset;
}

int utext_anim_disp(const struct utext *t) { return t->anim.disp; }

void utext_bar_units(const struct utext *t, int total_lines, int visible_rows,
                     int *out_total, int *out_visible, int *out_offset) {
    int ch = ugfx_char_h();
    if (ch <= 0) ch = 1;
    int max_px = (total_lines - visible_rows) * ch;
    if (max_px < 0) max_px = 0;
    // offset counts lines from the BOTTOM; content displaced DOWN
    // (disp > 0) is content that has not finished scrolling down, so the
    // thumb is still that much higher.
    int off_px = t->scroll_offset * ch + t->anim.disp;
    if (off_px < 0) off_px = 0;
    if (off_px > max_px) off_px = max_px;
    *out_total = total_lines * ch;
    *out_visible = visible_rows * ch;
    *out_offset = off_px;
}

void utext_metrics(struct utext *t, int w, int h,
                    int *out_total_lines, int *out_visible_rows) {
    int max_cols, visible_rows, x = 0;
    text_box(t, &x, &w);
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    clamp_scroll(t, t->wrap_cache.total_lines, visible_rows);
    if (out_total_lines) *out_total_lines = t->wrap_cache.total_lines;
    if (out_visible_rows) *out_visible_rows = visible_rows;
}

int utext_top_line(struct utext *t, int w, int h) {
    int max_cols, visible_rows, x = 0, lg = 0;
    text_box(t, &x, &w);
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    int first = clamp_scroll(t, t->wrap_cache.total_lines, visible_rows);
    line_begin_ex(t, max_cols, first, &lg);
    return lg;
}

int utext_top_index(struct utext *t, int w, int h) {
    int max_cols, visible_rows, x = 0;
    text_box(t, &x, &w);
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    return line_begin(t, max_cols, clamp_scroll(t, t->wrap_cache.total_lines, visible_rows));
}

void utext_scroll_index_to_top(struct utext *t, int w, int h, int index) {
    int max_cols, visible_rows, x = 0, line, col;
    text_box(t, &x, &w);
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    pos_of_index(t, max_cols, index, &line, &col);
    uui_scrollanim_cancel(&t->anim);
    t->scroll_offset = t->wrap_cache.total_lines - visible_rows - line;
    clamp_scroll(t, t->wrap_cache.total_lines, visible_rows);
}

void utext_reveal_cursor(struct utext *t, int w, int h) {
    int max_cols, visible_rows, x = 0;
    text_box(t, &x, &w);
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

    // A MARGIN, when asked, keeps lines in view around the caret (Vim's
    // 'scrolloff'); never more than leaves a row to stand on.
    int m = t->scroll_margin;
    if (m > (visible_rows - 1) / 2) m = (visible_rows - 1) / 2;
    if (m < 0) m = 0;
    int want = first;
    if (cl < first + m) want = cl - m;
    else if (cl >= first + visible_rows - m) want = cl - visible_rows + 1 + m;
    if (want < 0) want = 0;
    if (want > total - visible_rows) want = total - visible_rows > 0 ? total - visible_rows : 0;
    if (want == first) return;

    t->scroll_offset = total - visible_rows - want;
    clamp_scroll(t, total, visible_rows);
}

static int line_start(const struct utext *t, int pos);
static int line_end(const struct utext *t, int pos);

// A blend of two colours, `t` of 255 toward `b` -- the gutter's and the
// caret line's defaults, derived so a dark document gets dark ones.
static uint32_t mix(uint32_t a, uint32_t b, unsigned t) {
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        unsigned ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= ((ca * (255 - t) + cb * t) / 255) << sh;
    }
    return out;
}

void utext_draw(struct utext *t, struct ugfx_surface *s,
                 int x, int y, int w, int h,
                 uint32_t fg, uint32_t bg, uint32_t sel_bg, int show_cursor) {
    int gx = x, gw;
    text_box(t, &x, &w);
    gw = x - gx;
    int max_cols, visible_rows;
    grid(w, h, &max_cols, &visible_rows);
    int char_w = ugfx_char_w(), char_h = ugfx_char_h();

    uint32_t gut_bg = t->gutter_bg ? t->gutter_bg : mix(bg, fg, 10);
    uint32_t gut_fg = t->gutter_fg ? t->gutter_fg : mix(bg, fg, 110);
    uint32_t cur_bg = t->line_bg ? t->line_bg : mix(bg, sel_bg, 90);

    ugfx_fill_rect(s, x, y, w, h, bg);
    if (gw > 0) {
        ugfx_fill_rect(s, gx, y, gw, h, gut_bg);
        ugfx_fill_rect(s, gx + gw - 1, y, 1, h, mix(gut_bg, fg, 24));
    }

    wrap_ensure(t, max_cols);
    int first_line = clamp_scroll(t, t->wrap_cache.total_lines, visible_rows);

    int sel_start = 0, sel_end = 0;
    int has_sel = utext_sel_present(t);
    if (has_sel) utext_sel_range(t, &sel_start, &sel_end);

    // The caret's LOGICAL line, as a range of indices: a row whose span
    // starts inside it is one of its rows.
    int cur_ls = -1, cur_le = -1;
    if (t->line_highlight) {
        cur_ls = line_start(t, t->ed.cursor);
        cur_le = line_end(t, t->ed.cursor);
    }
    int digits = gw > 0 ? gutter_digits(t) : 0;

    // THE GLIDE: the lines are drawn `disp` px from where first_line puts
    // them for a few frames after a scroll (ui/uui_scrollanim.h), plus
    // the lines the displacement uncovers above or below, clipped to the
    // box. Off (disp stays 0) unless the host set `animate`.
    int disp = uui_scrollanim_sync(&t->anim, first_line * char_h);
    if (!t->animate) disp = 0;
    int extra = uui_scrollanim_extra_rows(disp, char_h);
    int start_line = disp > 0 ? first_line - extra : first_line;
    if (start_line < 0) start_line = 0;
    int rows_to_draw = visible_rows + 1 + extra;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, gx, y, gw + w, h);

    // ROW BY ROW, from the first VISIBLE line -- which is why a
    // document of any size costs a screenful of work per frame rather
    // than a documentful.
    int logical = 0;
    int i = line_begin_ex(t, max_cols, start_line, &logical);
    int mi = 0;   // the first mark that may still be ahead
    for (int row = 0; row < rows_to_draw && i <= t->count; row++) {
        int draw_end, next;
        line_span(t, max_cols, i, &draw_end, &next);

        int ry = y + (start_line - first_line + row) * char_h + disp;
        int on_cur = t->line_highlight && i >= cur_ls && i <= cur_le;
        uint32_t row_bg = on_cur ? cur_bg : bg;
        if (on_cur) ugfx_fill_rect(s, x, ry, w, char_h, row_bg);

        // The number, on the first row of a logical line only.
        if (gw > 0 && (i == 0 || t->buf[i - 1] == '\n')) {
            char num[12];
            int n = logical + 1, len = 0;
            do { num[len++] = (char)('0' + n % 10); n /= 10; } while (n && len < 11);
            for (int d = 0; d < len; d++)
                // text-measure-ok: a fixed grid by contract -- see the top of this file
                ugfx_draw_char(s, gx + (digits - d) * char_w, ry, num[d],
                               on_cur ? fg : gut_fg, gut_bg);
        }

        while (mi < t->mark_count && t->marks[mi].end <= i) mi++;
        int mk = mi;
        int vc = 0;   // the visual column, tabs expanded
        for (int k = i; k < draw_end; k++) {
            char ch = t->buf[k];
            int n = cells_at(t, ch, vc);
            int col = vc - t->hscroll;   // hscroll is 0 while wrapping
            vc += n;
            if (col + n <= 0) continue;
            if (col >= max_cols) break;
            while (mk < t->mark_count && t->marks[mk].end <= k) mk++;
            uint32_t cell = row_bg;
            if (has_sel && k >= sel_start && k < sel_end) cell = sel_bg;
            else if (mk < t->mark_count && t->marks[mk].start <= k) cell = t->marks[mk].bg;
            // text-measure-ok: a fixed grid by contract -- see the top of this file
            int rx = x + col * char_w;
            // text-measure-ok: a tab is n cells of the fixed grid -- see the top of this file
            if (cell != row_bg) ugfx_fill_rect(s, rx, ry, n * char_w, char_h, cell);
            if (ch == '\t' || (ch == ' ' && t->show_ws)) {
                // SPACES AND TABS, when asked: a centred dot for a space and
                // a rule to an arrowhead for a tab, faint (gedit's and
                // Kate's marks). Drawn, not glyphs: the grid font has none.
                if (t->show_ws) {
                    uint32_t ink = mix(cell, fg, 80);
                    int cy = ry + char_h / 2;
                    if (ch == ' ') ugfx_fill_rect(s, rx + char_w / 2 - 1, cy - 1, 2, 2, ink);
                    else {
                        // text-measure-ok: a tab is n cells of the fixed grid -- see the top of this file
                        int x0 = rx + 2, x1 = rx + n * char_w - 3;
                        ugfx_fill_rect(s, x0, cy, x1 - x0, 1, ink);
                        ugfx_fill_rect(s, x1 - 2, cy - 2, 1, 5, ink);
                        ugfx_fill_rect(s, x1 - 1, cy - 1, 1, 3, ink);
                    }
                }
                continue;
            }
            ugfx_draw_char(s, rx, ry, ch, fg, cell);
        }
        // A selection running THROUGH a line break should read as
        // reaching the end of the line, so paint one cell past the last
        // character rather than stopping short.
        if (has_sel && draw_end >= sel_start && draw_end < sel_end) {
            int col = col_of(t, i, draw_end) - t->hscroll;
            if (col >= 0 && col < max_cols)
                // text-measure-ok: same grid contract
                ugfx_fill_rect(s, x + col * char_w, ry, char_w, char_h, sel_bg);
        }

        if (last_span(t, draw_end, next) || next <= i) break;
        if (t->buf[next - 1] == '\n') logical++;
        i = next;
    }
    ugfx_clip_restore(s, &saved);

    if (show_cursor && uui_caret_visible()) {
        int cl, cc;
        pos_of_index(t, max_cols, t->ed.cursor, &cl, &cc);
        cc -= t->hscroll;
        if (cl >= first_line && cl - first_line < visible_rows &&
            cc >= 0 && cc <= max_cols)
            // text-measure-ok: same grid contract
            ugfx_fill_rect(s, x + cc * char_w, y + (cl - first_line) * char_h + disp,
                            CURSOR_BAR_W, char_h, fg);
    }
}

int utext_index_at_point(struct utext *t, int x, int y, int w, int h, int px, int py) {
    int max_cols, visible_rows;
    text_box(t, &x, &w);
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
            // The character whose cells hold the column; past a tab's
            // middle is the far side of it, as in every editor.
            int vc = 0;
            for (int k = i; k < draw_end; k++) {
                int n = cells_at(t, t->buf[k], vc);
                if (want_col < vc + n) return (n > 1 && want_col - vc >= (n + 1) / 2) ? k + 1 : k;
                vc += n;
            }
            return draw_end;
        }
        if (last_span(t, draw_end, next) || next <= i)
            return draw_end;   // clicked below the last line
        i = next;
    }
    return t->count;
}

int utext_widest_line(struct utext *t, int w, int h) {
    int max_cols, visible_rows, x = 0;
    text_box(t, &x, &w);
    grid(w, h, &max_cols, &visible_rows);
    wrap_ensure(t, max_cols);
    // While wrapping there is nothing to the right of the view by
    // construction, so the honest answer is the view's own width.
    if (t->wrap != UTEXT_WRAP_OFF) return max_cols;

    int widest = 0;
    for (int i = 0;;) {
        int draw_end, next;
        line_span(t, max_cols, i, &draw_end, &next);
        int w2 = col_of(t, i, draw_end);
        if (w2 > widest) widest = w2;
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

// A run at once: ONE move of the tail, not one per character.
static int raw_insert_n(struct utext *t, int at, const char *s, int n) {
    if (at < 0 || at > t->count) return 0;
    if (n > t->cap - t->count) n = t->cap - t->count;
    if (n <= 0) return 0;
    for (int i = t->count - 1; i >= at; i--) t->buf[i + n] = t->buf[i];
    for (int i = 0; i < n; i++) t->buf[at + i] = s[i];
    t->count += n;
    t->rev++;
    return n;
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

// **AN INSERT LEAVES NOTHING SELECTED**, and that is not cosmetic. A
// CLICK arms a selection (utext_sel_start) whose anchor is the caret,
// which is invisible only while the two are equal -- so anything that
// then moves the caret without clearing turns the text it passed over
// into a selection. Pasting after a click drew the pasted text
// highlighted; pressing Enter after one highlighted the newline. The
// shared keymap already clears on an ordinary keystroke
// (uui_edit.c's insert path), so this is that same rule for the two
// entry points that do not go through it.
static const struct uui_edit_ops UTEXT_EDIT_OPS;

void utext_insert(struct utext *t, char c) {
    if (uui_edit_insert(&t->ed, &UTEXT_EDIT_OPS, t, t->ed.cursor, c)) t->ed.cursor++;
    utext_sel_clear(t);
}

void utext_delete(struct utext *t) {
    uui_edit_erase(&t->ed, &UTEXT_EDIT_OPS, t, t->ed.cursor, t->ed.cursor + 1);
}

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
static int ed_insert_text(void *text, int i, const char *s, int n) {
    return raw_insert_n((struct utext *)text, i, s, n);
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
    .insert_text = ed_insert_text,
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
    // A paste over a selection is ONE step of the history.
    if (t->ed.undo) uui_undo_begin(t->ed.undo);
    utext_sel_delete(t);
    int put = 0;
    for (int i = 0; i < n;) {
        if (s[i] == '\r') { i++; continue; } // CRLF arrives as this buffer's own '\n'
        int j = i;
        while (j < n && s[j] != '\r') j++;
        int got = uui_edit_insert_text(&t->ed, &UTEXT_EDIT_OPS, t, t->ed.cursor, s + i, j - i);
        t->ed.cursor += got;
        put += got;
        if (got < j - i) break;   // full
        i = j;
    }
    if (t->ed.undo) uui_undo_end(t->ed.undo);
    utext_sel_clear(t);   // see utext_insert(): a paste selects nothing
    return put;
}

int utext_undo(struct utext *t) { return uui_edit_undo(&t->ed, &UTEXT_EDIT_OPS, t); }
int utext_redo(struct utext *t) { return uui_edit_redo(&t->ed, &UTEXT_EDIT_OPS, t); }
const struct uui_edit_ops *utext_edit_ops(void) { return &UTEXT_EDIT_OPS; }

int utext_key(struct utext *t, int key, unsigned mods) {
    if (!uui_key_is_shortcut(key, mods) && key == '\t' && t->tab_spaces && t->tab_width > 1) {
        // To the next stop, measured on the caret's logical line: what a
        // Tab would have filled, typed as spaces.
        int ls = line_start(t, t->ed.cursor);
        int n = t->tab_width - col_of(t, ls, t->ed.cursor) % t->tab_width;
        char sp[16];
        for (int i = 0; i < n && i < (int)sizeof sp; i++) sp[i] = ' ';
        utext_insert_text(t, sp, n < (int)sizeof sp ? n : (int)sizeof sp);
        return 1;
    }
    if (!uui_key_is_shortcut(key, mods) && (key == '\n' || key == '\r') && t->auto_indent) {
        // THE INDENT OF THE LINE ABOVE, up to the caret: Enter in the
        // middle of the indent does not invent more of it.
        int ls = line_start(t, t->ed.cursor);
        char ind[1 + 64];
        int n = 0;
        ind[n++] = '\n';
        for (int i = ls; i < t->ed.cursor && n < (int)sizeof ind; i++) {
            char c = t->buf[i];
            if (c != ' ' && c != '\t') break;
            ind[n++] = c;
        }
        utext_insert_text(t, ind, n);
        return 1;
    }
    return uui_edit_key(&t->ed, &UTEXT_EDIT_OPS, t, key, mods);
}
