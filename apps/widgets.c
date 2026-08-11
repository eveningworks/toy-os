#include "widgets.h"
#include "kapi.h"

int widget_hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg) {
    gfx_fill_rect(x, y, w, h, bg);
    if (!label) return;

    int len = (int)k_strlen(label);
    int lx = x + (w - len * gfx_char_w()) / 2;
    int ly = y + (h - gfx_char_h()) / 2;
    gfx_draw_string(lx, ly, label, fg, bg);
}

// ---- scrollback text widget (see widgets.h for the design writeup) ----

static struct scrollback_cell cell_at(const struct text_scrollback *tb, int i) {
    return tb->buf[(tb->start + i) % SCROLLBACK_CAP];
}

void widget_scrollback_init(struct text_scrollback *tb) {
    tb->start = 0;
    tb->count = 0;
    tb->cur_fg = VGA_LIGHT_GREY;
    tb->scroll_offset = 0;
    tb->cursor = 0;
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
// the wrapped line/column tb->cursor lands on -- the instant the walk
// reaches i == tb->cursor, whatever (line, col) it's at right then is
// the cursor's screen position, same wrapping rules as every other
// character. Captured at most once (the `captured` guard) since the
// walk continues past that point to still finish computing the total
// line count. i is allowed to reach tb->count (one past the last real
// character) specifically so a cursor sitting at the very end (the
// append-only case every caller used before this existed) is still
// captured correctly. Used by both widget_scrollback_metrics() and
// widget_scrollback_draw() so the two can never disagree about where
// the scroll range's boundaries -- or the cursor -- actually are.
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
                             uint32_t bg, int show_cursor) {
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

    // Pass 2: walk again, drawing only cells whose wrapped line falls
    // in [first_line, first_line + visible_rows).
    int line = 0, col = 0;
    for (int i = 0; i < tb->count; i++) {
        struct scrollback_cell cell = cell_at(tb, i);
        if (cell.ch == '\n') {
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
            gfx_draw_char(rx, ry, cell.ch, vga_color_rgb((enum vga_color)cell.fg), bg);
        }
        col++;
    }

    // Cursor -- drawn wherever tb->cursor actually is, as long as its
    // wrapped line falls within the visible window. For an append-only
    // caller tb->cursor always trails count, so cursor_line/cursor_col
    // here are exactly the old cur_line/cur_col (end-of-text) -- this
    // is that same behavior, just no longer assuming "the cursor" can
    // only ever be at the end.
    //
    // A thin CURSOR_BAR_W-px vertical bar at the cell's left edge, not a
    // solid full-cell block -- matches widget_textfield_draw()'s caret
    // (same width) and reads as a modern insert-point indicator instead
    // of an old-school block cursor that fully obscures whatever
    // character is underneath it.
    if (show_cursor && cursor_line >= first_line && cursor_line - first_line < visible_rows) {
        int rx = cx + cursor_col * char_w;
        int ry = cy + (cursor_line - first_line) * char_h;
        gfx_fill_rect(rx, ry, CURSOR_BAR_W, char_h, vga_color_rgb(tb->cur_fg));
    }
}

// ---- cursor-aware editing (see widgets.h's own comments) ----

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

// ---- vertical scrollbar widget (see widgets.h for the design writeup) ----

// Shared geometry: thumb height/position depend only on
// total_lines/visible_rows/scroll_offset (not on x, which callers vary
// to place the bar at the right edge of their content) -- computing it
// once here means draw()/hit()/thumb_rect()/offset_for_drag() can never
// disagree with each other about where the thumb is.
static void scrollbar_geometry(int y, int h, int total_lines, int visible_rows, int scroll_offset,
                                int *out_thumb_y, int *out_thumb_h, int *out_max_scroll) {
    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;

    int thumb_h = (total_lines > 0) ? h * visible_rows / total_lines : h;
    if (thumb_h < SCROLLBAR_MIN_THUMB_H) thumb_h = SCROLLBAR_MIN_THUMB_H;
    if (thumb_h > h) thumb_h = h;

    int track_range = h - thumb_h;
    int thumb_y = y;
    if (max_scroll > 0 && track_range > 0) {
        // scroll_offset == 0 (pinned to newest) -> thumb at the bottom;
        // scroll_offset == max_scroll (oldest) -> thumb at the top.
        thumb_y = y + track_range - track_range * scroll_offset / max_scroll;
    }

    *out_thumb_y = thumb_y;
    *out_thumb_h = thumb_h;
    *out_max_scroll = max_scroll;
}

void widget_scrollbar_draw(int x, int y, int w, int h, int total_lines, int visible_rows,
                            int scroll_offset, uint32_t track_bg, uint32_t thumb_bg) {
    gfx_fill_rect(x, y, w, h, track_bg);
    if (total_lines <= visible_rows) return; // everything fits -- no thumb to show

    int thumb_y, thumb_h, max_scroll;
    scrollbar_geometry(y, h, total_lines, visible_rows, scroll_offset, &thumb_y, &thumb_h, &max_scroll);
    gfx_fill_rect(x, thumb_y, w, thumb_h, thumb_bg);
}

enum scrollbar_zone widget_scrollbar_hit(int x, int y, int w, int h, int total_lines,
                                          int visible_rows, int scroll_offset, int px, int py) {
    if (!widget_hit(x, y, w, h, px, py)) return SCROLLBAR_ZONE_NONE;
    if (total_lines <= visible_rows) return SCROLLBAR_ZONE_NONE; // nothing to scroll -- no thumb, no paging

    int thumb_y, thumb_h, max_scroll;
    scrollbar_geometry(y, h, total_lines, visible_rows, scroll_offset, &thumb_y, &thumb_h, &max_scroll);

    if (py < thumb_y) return SCROLLBAR_ZONE_ABOVE;
    if (py >= thumb_y + thumb_h) return SCROLLBAR_ZONE_BELOW;
    return SCROLLBAR_ZONE_THUMB;
}

void widget_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                                  int scroll_offset, int *out_thumb_y, int *out_thumb_h) {
    int max_scroll;
    scrollbar_geometry(y, h, total_lines, visible_rows, scroll_offset, out_thumb_y, out_thumb_h, &max_scroll);
}

int widget_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                      int py, int grab_offset_in_thumb) {
    int thumb_y, thumb_h, max_scroll;
    // scroll_offset=0 here is arbitrary (only thumb_h/max_scroll are
    // used below -- thumb_y from this call is irrelevant, the drag
    // itself is what determines the thumb's position now).
    scrollbar_geometry(y, h, total_lines, visible_rows, 0, &thumb_y, &thumb_h, &max_scroll);
    if (max_scroll <= 0) return 0;

    int track_range = h - thumb_h;
    if (track_range <= 0) return 0;

    int new_thumb_y = py - grab_offset_in_thumb;
    if (new_thumb_y < y) new_thumb_y = y;
    if (new_thumb_y > y + track_range) new_thumb_y = y + track_range;

    int offset = max_scroll - (new_thumb_y - y) * max_scroll / track_range;
    if (offset < 0) offset = 0;
    if (offset > max_scroll) offset = max_scroll;
    return offset;
}

// ---- single-line text input widget (see widgets.h for the design writeup) ----

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
    // field (the bug this comment used to (incorrectly) claim couldn't
    // happen -- see docs/decisions.md). `visible` is how many
    // characters fit in the padded interior; when the field is active,
    // `start` slides right just far enough to keep the cursor inside
    // that window, so typing past the visible edge scrolls the same way
    // a real text input does -- an inactive field just shows the first
    // `visible` characters, plain truncation, no scroll needed since
    // there's no cursor to keep in view.
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

// ---- checkbox widget (see widgets.h for the design writeup) ----

#define CHECKBOX_LABEL_GAP 6

int widget_checkbox_width(int size, const char *label) {
    if (!label) return size;
    return size + CHECKBOX_LABEL_GAP + (int)k_strlen(label) * gfx_char_w();
}

void widget_checkbox_draw(int x, int y, int size, int checked, const char *label,
                           uint32_t bg, uint32_t fg) {
    gfx_draw_rect(x, y, size, size, fg);
    if (checked) {
        int inset = size / 4 > 0 ? size / 4 : 1;
        gfx_fill_rect(x + inset, y + inset, size - 2 * inset, size - 2 * inset, fg);
    }
    if (label) {
        int row_h = gfx_char_h();
        int label_y = y + (size - row_h) / 2;
        gfx_draw_string(x + size + CHECKBOX_LABEL_GAP, label_y, label, fg, bg);
    }
}

int widget_checkbox_hit(int x, int y, int size, const char *label, int px, int py) {
    int w = widget_checkbox_width(size, label);
    int row_h = gfx_char_h();
    int h = size > row_h ? size : row_h;
    return widget_hit(x, y, w, h, px, py);
}
