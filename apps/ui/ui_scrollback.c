// See ui_scrollback.h for the design writeup.
#include "ui_scrollback.h"
#include "ui_primitives.h" // CURSOR_BAR_W
#include "kapi.h"

static struct scrollback_cell cell_at(const struct text_scrollback *tb, int i) {
    return tb->buf[(tb->start + i) % SCROLLBACK_CAP];
}

void widget_scrollback_init(struct text_scrollback *tb) {
    tb->start = 0;
    tb->count = 0;
    tb->cur_fg = VGA_LIGHT_GREY;
    tb->scroll_offset = 0;
    tb->cursor = 0;
    tb->sel_anchor = 0;
    tb->sel_active = 0;
}

void widget_scrollback_putc(struct text_scrollback *tb, char c) {
    struct scrollback_cell cell = { .ch = c, .fg = (uint8_t)tb->cur_fg };
    if (tb->count < SCROLLBACK_CAP) {
        tb->buf[(tb->start + tb->count) % SCROLLBACK_CAP] = cell;
        tb->count++;
    } else {
        // Full -- overwrite the oldest cell in place and advance the
        // ring's head, same net effect as dropping the oldest character
        // and appending the new one, without shifting the whole buffer.
        tb->buf[tb->start] = cell;
        tb->start = (tb->start + 1) % SCROLLBACK_CAP;
    }
    tb->cursor = tb->count; // append-only API -- cursor always trails the write point
}

void widget_scrollback_backspace(struct text_scrollback *tb) {
    if (tb->count > 0) tb->count--;
    tb->cursor = tb->count;
}

void widget_scrollback_clear(struct text_scrollback *tb) {
    tb->start = 0;
    tb->count = 0;
    tb->scroll_offset = 0;
    tb->cursor = 0;
    tb->sel_anchor = 0;
    tb->sel_active = 0;
}

void widget_scrollback_set_color(struct text_scrollback *tb, enum vga_color fg) {
    tb->cur_fg = fg;
}

void widget_scrollback_scroll(struct text_scrollback *tb, int delta_lines) {
    tb->scroll_offset += delta_lines;
    if (tb->scroll_offset < 0) tb->scroll_offset = 0;
    // Upper bound depends on content width, which this function doesn't
    // know -- widget_scrollback_draw() clamps it down to the real max
    // on the next draw, so an over-large value here is harmless and
    // just temporary.
}

// Shared pass-1 walk: finds the wrapped line/column the write cursor
// ends up on (which gives the total wrapped line count) for the given
// column width, and clamps tb->scroll_offset against it. Also captures
// the wrapped line/column tb->cursor lands on. Used by both
// widget_scrollback_metrics() and widget_scrollback_draw() so the two
// can never disagree about where the scroll range's boundaries -- or
// the cursor -- actually are.
static void scrollback_measure(struct text_scrollback *tb, int max_cols, int visible_rows,
                                int *out_cur_line, int *out_cur_col,
                                int *out_cursor_line, int *out_cursor_col) {
    int cur_line = 0, cur_col = 0;
    int cursor_line = 0, cursor_col = 0;
    int captured = 0;
    for (int i = 0; i <= tb->count; i++) {
        if (i == tb->cursor && !captured) {
            cursor_line = cur_line;
            cursor_col = cur_col;
            captured = 1;
        }
        if (i == tb->count) break;
        char c = cell_at(tb, i).ch;
        if (c == '\n') {
            cur_line++;
            cur_col = 0;
            continue;
        }
        if (cur_col >= max_cols) {
            cur_line++;
            cur_col = 0;
        }
        cur_col++;
    }
    int total_lines = cur_line + 1;

    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;
    if (tb->scroll_offset > max_scroll) tb->scroll_offset = max_scroll;
    if (tb->scroll_offset < 0) tb->scroll_offset = 0;

    *out_cur_line = cur_line;
    *out_cur_col = cur_col;
    if (out_cursor_line) *out_cursor_line = cursor_line;
    if (out_cursor_col) *out_cursor_col = cursor_col;
}

void widget_scrollback_metrics(struct text_scrollback *tb, int cw, int ch,
                                int *out_total_lines, int *out_visible_rows) {
    int char_w = gfx_char_w(), char_h = gfx_char_h();
    int max_cols = cw / char_w;
    if (max_cols < 1) max_cols = 1;
    int visible_rows = ch / char_h;
    if (visible_rows < 1) visible_rows = 1;

    int cur_line, cur_col;
    scrollback_measure(tb, max_cols, visible_rows, &cur_line, &cur_col, 0, 0);

    if (out_total_lines) *out_total_lines = cur_line + 1;
    if (out_visible_rows) *out_visible_rows = visible_rows;
}

void widget_scrollback_draw(struct text_scrollback *tb, int cx, int cy, int cw, int ch,
                             uint32_t bg, uint32_t sel_bg, int show_cursor) {
    int char_w = gfx_char_w(), char_h = gfx_char_h();
    int max_cols = cw / char_w;
    if (max_cols < 1) max_cols = 1;
    int visible_rows = ch / char_h;
    if (visible_rows < 1) visible_rows = 1;

    gfx_fill_rect(cx, cy, cw, ch, bg);

    int cur_line, cur_col, cursor_line, cursor_col;
    scrollback_measure(tb, max_cols, visible_rows, &cur_line, &cur_col, &cursor_line, &cursor_col);
    int total_lines = cur_line + 1;

    int first_line = total_lines - visible_rows - tb->scroll_offset;
    if (first_line < 0) first_line = 0;

    int sel_start = 0, sel_end = 0;
    int has_sel = widget_scrollback_selection_present(tb);
    if (has_sel) widget_scrollback_selection_range(tb, &sel_start, &sel_end);

    // Pass 2: walk again, drawing only cells whose wrapped line falls
    // in [first_line, first_line + visible_rows).
    int line = 0, col = 0;
    for (int i = 0; i < tb->count; i++) {
        struct scrollback_cell cell = cell_at(tb, i);
        int selected = has_sel && i >= sel_start && i < sel_end;
        if (cell.ch == '\n') {
            // The newline itself isn't a glyph, but if it's inside the
            // selection, paint a thin highlight past the last real
            // character so a selection that runs to end-of-line still
            // reads as extending there, not stopping short.
            if (selected && line >= first_line && line - first_line < visible_rows) {
                int rx = cx + col * char_w;
                int ry = cy + (line - first_line) * char_h;
                gfx_fill_rect(rx, ry, char_w, char_h, sel_bg);
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
            int rx = cx + col * char_w;
            int ry = cy + (line - first_line) * char_h;
            uint32_t cell_bg = selected ? sel_bg : bg;
            if (selected) gfx_fill_rect(rx, ry, char_w, char_h, sel_bg);
            gfx_draw_char(rx, ry, cell.ch, vga_color_rgb((enum vga_color)cell.fg), cell_bg);
        }
        col++;
    }

    // Cursor -- drawn wherever tb->cursor actually is, as long as its
    // wrapped line falls within the visible window. A thin CURSOR_BAR_W
    // -px vertical bar at the cell's left edge, matching ui_textbox's
    // caret (same width).
    if (show_cursor && cursor_line >= first_line && cursor_line - first_line < visible_rows) {
        int rx = cx + cursor_col * char_w;
        int ry = cy + (cursor_line - first_line) * char_h;
        gfx_fill_rect(rx, ry, CURSOR_BAR_W, char_h, vga_color_rgb(tb->cur_fg));
    }
}

// ---- cursor-aware editing ----

// Index of the '\n' (or 0) that starts the line containing buffer
// position `pos`.
static int line_start(const struct text_scrollback *tb, int pos) {
    int i = pos;
    while (i > 0 && cell_at(tb, i - 1).ch != '\n') i--;
    return i;
}

// Index of the '\n' (or tb->count) that ends the line containing buffer
// position `pos`.
static int line_end(const struct text_scrollback *tb, int pos) {
    int i = pos;
    while (i < tb->count && cell_at(tb, i).ch != '\n') i++;
    return i;
}

void widget_scrollback_cursor_left(struct text_scrollback *tb) {
    if (tb->cursor > 0) tb->cursor--;
}

void widget_scrollback_cursor_right(struct text_scrollback *tb) {
    if (tb->cursor < tb->count) tb->cursor++;
}

void widget_scrollback_cursor_home(struct text_scrollback *tb) {
    tb->cursor = line_start(tb, tb->cursor);
}

void widget_scrollback_cursor_end(struct text_scrollback *tb) {
    tb->cursor = line_end(tb, tb->cursor);
}

void widget_scrollback_cursor_up(struct text_scrollback *tb) {
    int ls = line_start(tb, tb->cursor);
    if (ls == 0) { tb->cursor = 0; return; } // already on the first line
    int col = tb->cursor - ls;
    int prev_end = ls - 1; // the '\n' just before this line
    int prev_start = line_start(tb, prev_end);
    int prev_len = prev_end - prev_start;
    tb->cursor = prev_start + (col < prev_len ? col : prev_len);
}

void widget_scrollback_cursor_down(struct text_scrollback *tb) {
    int le = line_end(tb, tb->cursor);
    if (le >= tb->count) { tb->cursor = tb->count; return; } // already on the last line
    int ls = line_start(tb, tb->cursor);
    int col = tb->cursor - ls;
    int next_start = le + 1; // skip the '\n'
    int next_end = line_end(tb, next_start);
    int next_len = next_end - next_start;
    tb->cursor = next_start + (col < next_len ? col : next_len);
}

void widget_scrollback_insert_at_cursor(struct text_scrollback *tb, char c) {
    if (tb->count >= SCROLLBACK_CAP) return; // full -- see this call's own header comment
    // A selection left floating across an edit would desync from the
    // content it pointed at (indices shift under it) -- callers are
    // expected to call widget_scrollback_delete_selection() first when
    // one's present (that's what typing-over-a-selection wants anyway),
    // but clear it here too as a safety net rather than leave a stale
    // range around silently.
    tb->sel_active = 0;
    for (int i = tb->count; i > tb->cursor; i--) {
        tb->buf[(tb->start + i) % SCROLLBACK_CAP] = tb->buf[(tb->start + i - 1) % SCROLLBACK_CAP];
    }
    struct scrollback_cell cell = { .ch = c, .fg = (uint8_t)tb->cur_fg };
    tb->buf[(tb->start + tb->cursor) % SCROLLBACK_CAP] = cell;
    tb->count++;
    tb->cursor++;
}

void widget_scrollback_delete_at_cursor(struct text_scrollback *tb) {
    if (tb->cursor >= tb->count) return;
    tb->sel_active = 0; // see insert_at_cursor()'s comment
    for (int i = tb->cursor; i < tb->count - 1; i++) {
        tb->buf[(tb->start + i) % SCROLLBACK_CAP] = tb->buf[(tb->start + i + 1) % SCROLLBACK_CAP];
    }
    tb->count--;
}

void widget_scrollback_backspace_at_cursor(struct text_scrollback *tb) {
    if (tb->cursor == 0) return;
    tb->cursor--;
    widget_scrollback_delete_at_cursor(tb);
}

// ---- click-to-position + selection ----

int widget_scrollback_index_at_point(struct text_scrollback *tb, int cx, int cy, int cw, int ch,
                                      int px, int py) {
    int char_w = gfx_char_w(), char_h = gfx_char_h();
    int max_cols = cw / char_w;
    if (max_cols < 1) max_cols = 1;
    int visible_rows = ch / char_h;
    if (visible_rows < 1) visible_rows = 1;

    int cur_line, cur_col;
    scrollback_measure(tb, max_cols, visible_rows, &cur_line, &cur_col, 0, 0);
    int total_lines = cur_line + 1;
    int first_line = total_lines - visible_rows - tb->scroll_offset;
    if (first_line < 0) first_line = 0;

    int target_line = (py - cy) / char_h + first_line;
    int target_col = (px - cx) / char_w;
    if (target_line < 0) target_line = 0;
    if (target_col < 0) target_col = 0;

    // Walks the buffer with the exact same wrap/line accounting
    // scrollback_measure() uses to capture tb->cursor's (line,col) --
    // capturing THIS index's (line,col) at the top of each iteration,
    // before that iteration's own wrap/newline side effects, so a
    // resulting index always renders its cursor back at the same
    // (line,col) this function assumed. `last_on_target` tracks the
    // last index seen while still on target_line, so overshooting past
    // a short line's actual length lands at that line's end rather than
    // falling through to the next line.
    int line = 0, col = 0;
    int last_on_target = -1;
    for (int i = 0; i <= tb->count; i++) {
        if (line == target_line) {
            last_on_target = i;
            if (col >= target_col) return i;
        } else if (line > target_line) {
            break;
        }
        if (i == tb->count) break;
        char c = cell_at(tb, i).ch;
        if (c == '\n') {
            line++;
            col = 0;
        } else {
            if (col >= max_cols) { line++; col = 0; }
            col++;
        }
    }
    return (last_on_target >= 0) ? last_on_target : tb->count;
}

void widget_scrollback_selection_start(struct text_scrollback *tb) {
    tb->sel_anchor = tb->cursor;
    tb->sel_active = 1;
}

void widget_scrollback_selection_clear(struct text_scrollback *tb) {
    tb->sel_active = 0;
}

int widget_scrollback_selection_present(const struct text_scrollback *tb) {
    return tb->sel_active && tb->sel_anchor != tb->cursor;
}

void widget_scrollback_selection_range(const struct text_scrollback *tb, int *out_start, int *out_end) {
    if (!widget_scrollback_selection_present(tb)) {
        *out_start = tb->cursor;
        *out_end = tb->cursor;
        return;
    }
    if (tb->sel_anchor < tb->cursor) {
        *out_start = tb->sel_anchor;
        *out_end = tb->cursor;
    } else {
        *out_start = tb->cursor;
        *out_end = tb->sel_anchor;
    }
}

void widget_scrollback_delete_selection(struct text_scrollback *tb) {
    if (!widget_scrollback_selection_present(tb)) return;
    int start, end;
    widget_scrollback_selection_range(tb, &start, &end);
    tb->cursor = end;
    tb->sel_active = 0;
    // backspace_at_cursor()/delete_at_cursor() shift the ring one cell
    // at a time -- fine at SCROLLBACK_CAP's size, same tradeoff every
    // other cursor-aware edit here already makes.
    for (int i = end; i > start; i--) widget_scrollback_backspace_at_cursor(tb);
}
