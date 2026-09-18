// table -- rows in columns, with a header. See ui/uui_table.h.
#include "ui/uui_table.h"
#include "ui/uui_widget.h"
#include "ui/uui_scrollbar.h" // uui_scrollbar_natural_size()
#include <string.h>
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// Left/right inset inside a column. Hoisted so natural_size() reserves
// exactly what the draw uses -- the "one geometry, shared" rule the
// scrollbar already follows.
#define UUI_TABLE_PAD_X 4

// Longest cell text this widget will render. A cell is one column wide
// and gets clipped anyway, so this bounds the app's formatting rather
// than the layout.
#define UUI_TABLE_CELL_MAX 96

// A fixed column's width, RESERVED IN DIGITS rather than in the widest
// glyph -- `uui_statusbar.c`'s `fixed_w()` in full, for the same reason
// and with the same consequence when it is got wrong: `char_w` is the
// widest advance in the face, so fixed columns over-reserve on a
// proportional one and eat the width the stretch columns needed.
// A column declaring a character count is nearly always reserving room
// for a number, and digits are one width in any sane face.
//
// One derivation, because column_rect() and natural_size() must agree
// or the table draws its cells outside the columns it measured.
static int col_fixed_w(int chars) {
    int per = ugfx_char_advance('0');
    if (per <= 0) per = ugfx_char_w();
    return chars * per;
}

void uui_table_init(struct uui_table *t, int x, int y, int w, int h,
                     const struct uui_table_column *cols, int col_count,
                     uui_table_cell_fn cell, void *ctx) {
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->cols = cols;
    t->col_count = col_count;
    t->row_count = 0;
    t->cell = cell;
    t->ctx = ctx;
    t->show_header = 1;
    t->selected = -1;
    t->hovered = -1;
    t->top = 0;
    t->row_h = 0; // derive from the font
    // THE TOOLKIT'S OWN DEFAULT, not a pixel count. uui_scrollbar.h
    // asks every widget to take its width from there so the bar tracks
    // the font size and the toolkit's bars keep matching each other;
    // this one picked 8, which is the case that header names. At the
    // default face that left a thumb the maintainer could not reliably
    // grab -- the same complaint that widened uui_textview's.
    uui_scrollbar_natural_size(&t->bar_w, 0);
    t->thumb_grab = -1;
    // Sorting off until an app supplies a comparator, so a table that
    // says nothing about sorting behaves exactly as it did before.
    t->compare = 0;
    t->tint = 0;
    t->fade = 0;
    t->sort_col = UUI_TABLE_UNSORTED;
    t->sort_dir = 1;
    t->order_rows = 0;
    // See the header: column 0 by default, so a table that says nothing
    // about searching still answers the keyboard.
    t->seek_col = 0;
    uui_seek_reset(&t->seek);

    // UNSET means "ask the theme when you draw" (utheme.h). An app
    // setting any of these still wins.
    t->bg = UUI_COLOR_UNSET;
    t->fg = UUI_COLOR_UNSET;
    t->sel_bg = UUI_COLOR_UNSET;
    t->sel_fg = UUI_COLOR_UNSET;
    t->head_bg = UUI_COLOR_UNSET;
    // A header's ink is a step lighter than body text and the palette
    // has no role for it yet; left a literal rather than mapped to
    // `text`, which would darken every table header today.
    t->head_fg = ugfx_rgb(40, 40, 40);
    t->grid = UUI_COLOR_UNSET;
    t->track_bg = UUI_COLOR_UNSET;
    t->thumb_bg = UUI_COLOR_UNSET;
}


// --- colours, resolved when we DRAW ----------------------------------
//
// Not at init: a widget built before a theme change would otherwise
// keep the old palette forever, and every app builds its widgets once.
// See utheme.h's UUI_COLOR.
uint32_t uui_table_c_bg(const struct uui_table *t)      { return UUI_COLOR(t->bg, UTHEME_WHITE); }
uint32_t uui_table_c_fg(const struct uui_table *t)      { return UUI_COLOR(t->fg, UTHEME_TEXT); }
uint32_t uui_table_c_sel_bg(const struct uui_table *t)  { return UUI_COLOR(t->sel_bg, UTHEME_SELECTION); }
uint32_t uui_table_c_sel_fg(const struct uui_table *t)  { return UUI_COLOR(t->sel_fg, UTHEME_TEXT); }
uint32_t uui_table_c_head_bg(const struct uui_table *t) { return UUI_COLOR(t->head_bg, UTHEME_BUTTON_BG); }
uint32_t uui_table_c_grid(const struct uui_table *t)    { return UUI_COLOR(t->grid, UTHEME_SEPARATOR); }
uint32_t uui_table_c_track_bg(const struct uui_table *t)   { return UUI_COLOR(t->track_bg, UTHEME_BUTTON_BG); }
uint32_t uui_table_c_thumb_bg(const struct uui_table *t)   { return UUI_COLOR(t->thumb_bg, UTHEME_OUTLINE); }

int uui_table_row_h(const struct uui_table *t) {
    return t->row_h > 0 ? t->row_h : ugfx_char_h() + 4;
}

int uui_table_header_h(const struct uui_table *t) {
    return t->show_header ? uui_table_row_h(t) : 0;
}

void uui_table_set_seek_col(struct uui_table *t, int col) {
    t->seek_col = (col < t->col_count) ? col : -1;
    uui_seek_reset(&t->seek);
}

void uui_table_set_header(struct uui_table *t, int show) {
    t->show_header = show ? 1 : 0;
}

int uui_table_visible_rows(const struct uui_table *t) {
    int rh = uui_table_row_h(t);
    int avail = t->h - uui_table_header_h(t);
    int n = rh > 0 ? avail / rh : 0;
    return n > 0 ? n : 1;
}

int uui_table_scrollbar_visible(const struct uui_table *t) {
    return t->row_count > uui_table_visible_rows(t);
}

// Clamps `top` so the view always fills when it can -- a table scrolled
// past its end shows blank rows under real ones, which reads as missing
// data rather than as the end of the list.
static void table_clamp(struct uui_table *t) {
    int vis = uui_table_visible_rows(t);
    int max_top = t->row_count > vis ? t->row_count - vis : 0;
    if (t->top > max_top) t->top = max_top;
    if (t->top < 0) t->top = 0;
}

// --- sorting ----------------------------------------------------------
//
// See uui_table.h for why the app compares and the widget permutes.
// Everything below maintains `order`, a view-position -> app-row map;
// identity when there is no sort, so every caller can go through it
// unconditionally and there is no second, unsorted code path to keep
// in step.

static void order_identity(struct uui_table *t) {
    int n = t->row_count;
    if (n > UUI_TABLE_MAX_ROWS) n = UUI_TABLE_MAX_ROWS;
    for (int i = 0; i < n; i++) t->order[i] = i;
    t->order_rows = t->row_count;
}

static void order_rebuild(struct uui_table *t) {
    order_identity(t);
    if (!t->compare || t->sort_col < 0 || t->sort_col >= t->col_count) return;

    int n = t->row_count;
    if (n > UUI_TABLE_MAX_ROWS) n = UUI_TABLE_MAX_ROWS;

    // Insertion sort: STABLE, which is what makes a second sort on a
    // different column keep the previous column's order within ties --
    // the behaviour every desktop table has. Also correct at n = 0 and
    // 1 with no special cases, and n is bounded by UUI_TABLE_MAX_ROWS,
    // so the quadratic worst case is 256 rows of a handful of
    // comparisons. There is no qsort in this toolkit.
    for (int i = 1; i < n; i++) {
        int v = t->order[i];
        int j = i - 1;
        while (j >= 0) {
            int c = t->compare(t->ctx, t->order[j], v, t->sort_col) * t->sort_dir;
            if (c <= 0) break;   // <= keeps equal elements in place: stable
            t->order[j + 1] = t->order[j];
            j--;
        }
        t->order[j + 1] = v;
    }
}

int uui_table_source_row(const struct uui_table *t, int view_row) {
    if (view_row < 0 || view_row >= t->row_count) return -1;
    // Past the permutation's capacity, or built for a different row
    // count: fall back to identity rather than reading a stale slot.
    // Showing the tail unsorted is a visible oddity; indexing the wrong
    // row is a silent one.
    if (view_row >= UUI_TABLE_MAX_ROWS || t->order_rows != t->row_count) return view_row;
    return t->order[view_row];
}

int uui_table_view_row(const struct uui_table *t, int source_row) {
    if (source_row < 0 || source_row >= t->row_count) return -1;
    if (source_row >= UUI_TABLE_MAX_ROWS || t->order_rows != t->row_count) return source_row;
    for (int i = 0; i < t->row_count && i < UUI_TABLE_MAX_ROWS; i++) {
        if (t->order[i] == source_row) return i;
    }
    return source_row;
}

void uui_table_set_compare(struct uui_table *t, uui_table_cmp_fn compare) {
    t->compare = compare;
    order_rebuild(t);
}

void uui_table_set_fade(struct uui_table *t, uui_table_fade_fn fade) {
    t->fade = fade;
}

void uui_table_set_tint(struct uui_table *t, uui_table_tint_fn tint) {
    t->tint = tint;
}

void uui_table_set_sort(struct uui_table *t, int col, int dir) {
    if (col < UUI_TABLE_UNSORTED || col >= t->col_count) return;
    t->sort_col = col;
    t->sort_dir = dir < 0 ? -1 : 1;
    order_rebuild(t);
}

void uui_table_set_rows(struct uui_table *t, int row_count) {
    t->row_count = row_count;
    // Both of these move on every refresh in a task manager: processes
    // come and go under a live view. Clamping HERE is why the app never
    // has to think about it.
    if (t->selected >= row_count) t->selected = row_count > 0 ? row_count - 1 : -1;
    if (t->hovered >= row_count) t->hovered = -1;
    // The app just told us its data changed, so the permutation is what
    // is stale. Rebuilding HERE is why an app that already calls this
    // after a refresh needs no sorting hook of its own -- Task Manager
    // re-reads the process table several times a second and says
    // nothing about sorting beyond supplying the comparator.
    order_rebuild(t);
    table_clamp(t);
}

// Width available to the columns -- the whole widget minus the
// scrollbar, when one is showing.
static int table_content_w(const struct uui_table *t) {
    return t->w - (uui_table_scrollbar_visible(t) ? t->bar_w : 0);
}

void uui_table_column_rect(const struct uui_table *t, int col,
                            int *out_x, int *out_w) {
    if (out_x) *out_x = t->x;
    if (out_w) *out_w = 0;
    if (col < 0 || col >= t->col_count) return;

    int fixed = 0, stretch = 0;
    for (int i = 0; i < t->col_count; i++) {
        if (t->cols[i].width_chars > 0) fixed += col_fixed_w(t->cols[i].width_chars);
        else stretch++;
    }

    int spare = table_content_w(t) - fixed;
    if (spare < 0) spare = 0;
    // Split evenly between stretch columns; the LAST stretch column
    // takes the rounding remainder so the columns exactly fill the
    // width rather than leaving a one-pixel gap that looks like a bug.
    int each = stretch > 0 ? spare / stretch : 0;

    int x = t->x, seen_stretch = 0, width = 0;
    for (int i = 0; i <= col; i++) {
        if (t->cols[i].width_chars > 0) {
            width = col_fixed_w(t->cols[i].width_chars);
        } else {
            seen_stretch++;
            width = (seen_stretch == stretch) ? spare - each * (stretch - 1) : each;
        }
        if (i < col) x += width;
    }

    if (out_x) *out_x = x;
    if (out_w) *out_w = width;
}

// One cell's text, drawn clipped to its column and aligned per the
// column's own rule.
static void draw_cell(struct ugfx_surface *s, const struct uui_table *t,
                       int col, int row, int ry, uint32_t fg, uint32_t bg) {
    char buf[UUI_TABLE_CELL_MAX];
    buf[0] = '\0';
    if (t->cell) t->cell(t->ctx, row, col, buf, (int)sizeof buf);
    if (!buf[0]) return;

    int cx, cw;
    uui_table_column_rect(t, col, &cx, &cw);
    int avail = cw - UUI_TABLE_PAD_X * 2;
    if (avail <= 0) return;

    int tx = cx + UUI_TABLE_PAD_X;
    if (t->cols[col].align == UUI_TALIGN_RIGHT) {
        int tw = ugfx_text_width(buf);
        if (tw < avail) tx = cx + cw - UUI_TABLE_PAD_X - tw;
    }

    int ty = ry + (uui_table_row_h(t) - ugfx_char_h()) / 2;
    // Clipped, always: a column is a fixed box and ugfx_draw_string()
    // does not clip (docs/gui-guidelines.md's oldest trap, which has
    // produced the same overlap bug in two separate files).
    ugfx_draw_string_clipped(s, tx, ty, avail, buf, fg, bg);
}

// The header row: column titles, the sort arrow, and the rule under
// them. Split out because it is skipped ENTIRELY when the header is
// hidden (uui_table_set_header()) -- the rule below would otherwise
// land one pixel above the widget's own rect.
static void table_draw_header(struct ugfx_surface *s, const struct uui_table *t, int hh) {
    ugfx_fill_rect(s, t->x, t->y, t->w, hh, uui_table_c_head_bg(t));
    for (int c = 0; c < t->col_count; c++) {
        int cx, cw;
        uui_table_column_rect(t, c, &cx, &cw);
        int avail = cw - UUI_TABLE_PAD_X * 2;
        if (avail <= 0) continue;

        // The arrow's reservation is taken out FIRST, before the title
        // is positioned -- not after. Computing it afterwards is what
        // made a RIGHT-aligned title (PID, CPU, Memory) draw hard
        // against the column's right edge, which is exactly where the
        // arrow goes, so the arrow sat on top of the last character.
        // Reserving width for the clip is not enough on its own: a
        // right-aligned string is positioned from the right edge, so
        // the edge itself has to move.
        int arrow = 0;
        if (t->compare && c == t->sort_col) {
            arrow = ugfx_char_w() + UUI_TABLE_PAD_X; // glyph plus a gap
        }
        int text_avail = avail - arrow;
        if (text_avail <= 0) continue; // no room for a title at all

        int tx = cx + UUI_TABLE_PAD_X;
        const char *title = t->cols[c].title ? t->cols[c].title : "";
        if (t->cols[c].align == UUI_TALIGN_RIGHT) {
            int tw = ugfx_text_width(title);
            if (tw < text_avail) tx = cx + cw - UUI_TABLE_PAD_X - arrow - tw;
        }
        ugfx_draw_string_clipped(s, tx, t->y + (hh - ugfx_char_h()) / 2,
                                  text_avail, title, t->head_fg, uui_table_c_head_bg(t));
        if (arrow) {
            // Drawn as stacked rows rather than a glyph: the baked font
            // has no arrow character, and a triangle built from the
            // font's own cell size stays font-derived per
            // docs/gui-guidelines.md.
            int aw = ugfx_char_w();
            int ah = aw / 2 > 2 ? aw / 2 : 2;
            int ax = cx + cw - UUI_TABLE_PAD_X - aw;
            int ay = t->y + (hh - ah) / 2;
            for (int r = 0; r < ah; r++) {
                // Ascending points UP, which is the direction the
                // smallest value is: same as Windows and KDE.
                int row = (t->sort_dir > 0) ? r : ah - 1 - r;
                int inset = (ah - 1 - row) * aw / (2 * ah);
                int rw = aw - 2 * inset;
                if (rw < 1) rw = 1;
                ugfx_fill_rect(s, ax + inset, ay + r, rw, 1, t->head_fg);
            }
        }
        // Column separator, header only -- a full grid turns a dense
        // table into graph paper, and every desktop table draws the
        // header rule and leaves the body clean.
        if (c > 0) ugfx_fill_rect(s, cx, t->y, 1, hh, uui_table_c_grid(t));
    }
    ugfx_fill_rect(s, t->x, t->y + hh - 1, t->w, 1, uui_table_c_grid(t));
}

void uui_table_draw(struct ugfx_surface *s, const struct uui_table *t) {
    int rh = uui_table_row_h(t);
    int hh = uui_table_header_h(t);
    int vis = uui_table_visible_rows(t);
    int bar = uui_table_scrollbar_visible(t) ? t->bar_w : 0;

    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, uui_table_c_bg(t));

    // --- header ---
    if (hh > 0) table_draw_header(s, t, hh);

    // --- rows ---
    for (int i = 0; i < vis; i++) {
        if (t->top + i >= t->row_count) break;
        // The app's row for this SCREEN position. `selected` and
        // `hovered` are app rows too, so the comparisons below are
        // apples to apples and a selection survives a re-sort.
        int idx = uui_table_source_row(t, t->top + i);
        if (idx < 0) break;
        int ry = t->y + hh + i * rh;

        uint32_t rbg = uui_table_c_bg(t), rfg = uui_table_c_fg(t);
        uint32_t tint = t->tint ? t->tint(t->ctx, idx) : 0;
        if (idx == t->selected) { rbg = uui_table_c_sel_bg(t); rfg = uui_table_c_sel_fg(t); }
        else if (idx == t->hovered) { rbg = uui_state_bg(uui_table_c_bg(t), UUI_STATE_HOVER); }
        else if (tint) { rbg = tint; }

        // HALFWAY TO THE BACKGROUND, which is what a cut looks like on
        // every desktop. Applied after the state colours so a faded row
        // that is also selected fades from the SELECTION's text colour.
        if (t->fade && t->fade(t->ctx, idx)) rfg = ugfx_blend(rfg, rbg, 128);

        if (rbg != uui_table_c_bg(t)) ugfx_fill_rect(s, t->x, ry, t->w - bar, rh, rbg);
        for (int c = 0; c < t->col_count; c++) draw_cell(s, t, c, idx, ry, rfg, rbg);
    }

    if (bar) {
        // Beside the ROWS, not the header -- a bar that started at the
        // top would let the thumb sit next to column titles it cannot
        // scroll.
        uui_scrollbar_draw(s, t->x + t->w - bar, t->y + hh, bar, t->h - hh,
                            t->row_count, vis, t->row_count - vis - t->top,
                            uui_table_c_track_bg(t), uui_table_c_thumb_bg(t), 0);
    }

    // On the selected ROW, because that is what the arrows move. Round
    // the whole widget instead when the selection is scrolled out of
    // view or there is none -- focus is still here, and an indicator
    // that disappears is the thing this exists to prevent. `selected`
    // is an app row and the loop above walks VIEW positions, so it has
    // to be converted, the same rule uui_table_key()'s arrows follow.
    if (t->focused) {
        int view = uui_table_view_row(t, t->selected);
        int i = view - t->top;
        if (view >= 0 && i >= 0 && i < vis)
            uui_focus_ring(s, t->x, t->y + hh + i * rh, t->w - bar, rh);
        else
            uui_focus_ring(s, t->x, t->y, t->w, t->h);
    }
}

void uui_table_natural_size(const struct uui_table *t, int *out_w, int *out_h) {
    if (out_w) {
        int total = 0;
        for (int i = 0; i < t->col_count; i++) {
            int chars = t->cols[i].width_chars;
            if (chars <= 0) {
                // A stretch column has no width of its own; ask for
                // enough to show its title, and let the layout give
                // more.
                int tw = ugfx_text_width(t->cols[i].title ? t->cols[i].title : "");
                total += tw + UUI_TABLE_PAD_X * 2;
            } else {
                total += col_fixed_w(chars);
            }
        }
        *out_w = total + t->bar_w;
    }
    // Header plus a few rows: a preferred MINIMUM, not the whole table.
    // Asking for every row would make a window with 60 processes in it
    // taller than the screen.
    if (out_h) *out_h = uui_table_header_h(t) + uui_table_row_h(t) * 6;
}

int uui_table_header_hit(const struct uui_table *t, int cx, int cy) {
    if (!uui_hit(t->x, t->y, t->w, uui_table_header_h(t), cx, cy)) return -1;
    for (int c = 0; c < t->col_count; c++) {
        int colx, colw;
        uui_table_column_rect(t, c, &colx, &colw);
        if (cx >= colx && cx < colx + colw) return c;
    }
    return -1;
}

int uui_table_hit(const struct uui_table *t, int cx, int cy) {
    int bar = uui_table_scrollbar_visible(t) ? t->bar_w : 0;
    int hh = uui_table_header_h(t);
    if (!uui_hit(t->x, t->y + hh, t->w - bar, t->h - hh, cx, cy)) return -1;
    int rh = uui_table_row_h(t);
    int view = t->top + (cy - t->y - hh) / rh;
    if (view < 0 || view >= t->row_count) return -1;
    // The APP's row, not the screen position -- every public row index
    // on this widget means the same thing (uui_table.h).
    return uui_table_source_row(t, view);
}

int uui_table_hover(struct uui_table *t, int cx, int cy) {
    int idx = uui_table_hit(t, cx, cy);
    if (idx == t->hovered) return 0;
    t->hovered = idx;
    return 1;
}

int uui_table_click(struct uui_table *t, int cx, int cy) {
    // A header click sorts, and only when the app opted in by supplying
    // a comparator -- otherwise the header stays inert rather than
    // becoming a control that highlights and does nothing.
    if (t->compare) {
        int col = uui_table_header_hit(t, cx, cy);
        if (col >= 0) {
            // Clicking the SAME column reverses; a different one starts
            // ascending. That is the rule on every desktop, and the
            // reason it is here rather than in each app.
            if (col == t->sort_col) uui_table_set_sort(t, col, -t->sort_dir);
            else                    uui_table_set_sort(t, col, 1);
            return 1;
        }
    }

    int idx = uui_table_hit(t, cx, cy);
    if (idx < 0 || idx == t->selected) return 0;
    t->selected = idx;
    return 1;
}

// --- the scrollbar's own input ---------------------------------------
//
// Same offset inversion as the listbox: `top` counts from the top,
// while the scrollbar's offset counts from the BOTTOM. draw() converts
// one way; everything here converts back the same way or the thumb
// tracks the cursor upside down.
static int table_bar_x(const struct uui_table *t) {
    return t->x + t->w - t->bar_w;
}

static int table_offset(const struct uui_table *t) {
    return t->row_count - uui_table_visible_rows(t) - t->top;
}

static int table_set_offset(struct uui_table *t, int offset) {
    int before = t->top;
    t->top = t->row_count - uui_table_visible_rows(t) - offset;
    table_clamp(t);
    return t->top != before;
}

int uui_table_press(struct uui_table *t, int cx, int cy) {
    if (!uui_table_scrollbar_visible(t)) return 0;
    int hh = uui_table_header_h(t);
    int bx = table_bar_x(t);
    if (cx < bx || cx >= t->x + t->w) return 0;
    if (cy < t->y + hh || cy >= t->y + t->h) return 0;

    int vis = uui_table_visible_rows(t);
    int off = table_offset(t);
    enum uui_scrollbar_zone zone =
        uui_scrollbar_hit(bx, t->y + hh, t->bar_w, t->h - hh,
                           t->row_count, vis, off, cx, cy, 0);

    if (zone == UUI_SB_THUMB) {
        int thumb_y, thumb_h;
        uui_scrollbar_thumb_rect(t->y + hh, t->h - hh, t->row_count, vis, off,
                                  &thumb_y, &thumb_h, t->bar_w, 0);
        // The offset WITHIN the thumb, so it tracks the cursor rather
        // than snapping its top to it.
        t->thumb_grab = cy - thumb_y;
        return 1;
    }

    int page = vis > 1 ? vis - 1 : 1;
    if (zone == UUI_SB_ABOVE) table_set_offset(t, off + page);
    else if (zone == UUI_SB_BELOW) table_set_offset(t, off - page);
    else return 0;
    return 1;
}

int uui_table_drag(struct uui_table *t, int cx, int cy) {
    (void)cx;
    if (t->thumb_grab < 0) return 0;
    int hh = uui_table_header_h(t);
    int vis = uui_table_visible_rows(t);
    int off = uui_scrollbar_offset_for_drag(t->y + hh, t->h - hh, t->row_count,
                                             vis, cy, t->thumb_grab, t->bar_w, 0);
    return table_set_offset(t, off);
}

void uui_table_drag_end(struct uui_table *t) {
    t->thumb_grab = -1;
}

int uui_table_wheel(struct uui_table *t, int notches) {
    int before = t->top;
    // Scrolling must NOT change the selection -- a wheel over a table is
    // navigation, not a choice.
    t->top -= notches * 3;
    table_clamp(t);
    return t->top != before;
}

static void table_reveal(struct uui_table *t) {
    int vis = uui_table_visible_rows(t);
    // `top` is a VIEW offset and `selected` is an APP row, so the two
    // are only interchangeable while the table is unsorted. Comparing
    // them directly scrolls to wherever that app row happens to sit in
    // the underlying data, which once sorted is not where it is drawn.
    int view = uui_table_view_row(t, t->selected);
    if (view < 0) return;
    if (view < t->top) t->top = view;
    else if (view >= t->top + vis) t->top = view - vis + 1;
    table_clamp(t);
}

// --- type-ahead --------------------------------------------------------
//
// The search is ui/uui_seek.h's, shared with uui_listbox. What is added
// here is the index space: the callback is handed a VIEW position and
// pulls the cell for the app row behind it, so a sorted table cycles in
// the order on SCREEN. Walking the app's order instead would make the
// same letter jump around a sorted table, which is the bug
// uui_table_key()'s arrow handling already avoids.

static void tb_seek_text(void *ctx, int view_row, char *out, int cap) {
    const struct uui_table *t = (const struct uui_table *)ctx;
    out[0] = '\0';
    if (!t->cell || t->seek_col < 0) return;
    int row = uui_table_source_row(t, view_row);
    if (row < 0) return;
    t->cell(t->ctx, row, t->seek_col, out, cap);
}

// Returns 1 if the selection moved.
static int table_seek(struct uui_table *t, int key) {
    if (t->seek_col < 0 || !t->cell) return 0;
    int view = uui_table_view_row(t, t->selected);
    int idx = uui_seek_key(&t->seek, key, t->row_count, view,
                            tb_seek_text, t);
    if (idx < 0) return 0;
    int row = uui_table_source_row(t, idx);
    if (row < 0 || row == t->selected) return 0;
    t->selected = row;
    table_reveal(t);
    return 1;
}

int uui_table_key(struct uui_table *t, int key) {
    if (t->row_count <= 0) return 0;
    int before = t->selected;
    int vis = uui_table_visible_rows(t);

    // Motion happens in VIEW order and is converted back at the end:
    // Down must move to the row visibly BELOW, which is not
    // selected + 1 once the table is sorted. Doing this in app indices
    // is the bug that makes arrow keys jump around a sorted table.
    int view = uui_table_view_row(t, t->selected);
    if (view < 0) view = 0;

    if (key == KEY_ARROW_UP)        { if (view > 0) view--; }
    else if (key == KEY_ARROW_DOWN) { if (view < t->row_count - 1) view++; }
    else if (key == KEY_HOME)       { view = 0; }
    else if (key == KEY_END)        { view = t->row_count - 1; }
    else if (key == KEY_PAGE_UP)    { view -= vis; if (view < 0) view = 0; }
    else if (key == KEY_PAGE_DOWN)  { view += vis;
                                       if (view >= t->row_count) view = t->row_count - 1; }
    // A PRINTABLE KEY IS A SEARCH, not a keystroke to pass on.
    else if (uui_seek_is_key(key)) return table_seek(t, key);
    else return 0;

    if (view < 0) view = 0;
    t->selected = uui_table_source_row(t, view);
    if (t->selected < 0) t->selected = 0;
    // A movement key ends a search in progress, so the next letter
    // starts a new one rather than extending an abandoned prefix.
    uui_seek_reset(&t->seek);
    table_reveal(t);
    return t->selected != before;
}

// --- the ops table ----------------------------------------------------
//
// Filled in for pointer input too, so an app declares a table and never
// mentions input again -- see ui/uui_widget.h on why that matters.
static void tb_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_table_natural_size((const struct uui_table *)w, out_w, out_h);
}

static void tb_ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_table *t = (struct uui_table *)w;
    t->x = x; t->y = y; t->w = width; t->h = height;
    // The visible row count just changed, so `top` may now point past a
    // valid first row. This is what makes a RESIZE reflow correctly
    // instead of showing a blank band under the last row.
    table_clamp(t);
}

static void tb_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_table_draw(s, (const struct uui_table *)w);
}

// `>= 0`, NOT the row index. The router tests this as a BOOLEAN
// (`!it->ops->hit(...)` in ui/uui_route.c), so returning the index makes
// ROW 0 report "not hit" -- it is the one row whose index is falsey, so
// the first row of the table silently cannot be selected while every
// other row works. See uui_listbox.c, which had the identical bug.
static int tb_ops_hit(const void *w, int cx, int cy) {
    // The WHOLE widget, not just its rows. uui_table_hit() deliberately
    // excludes the header and the scrollbar column -- it answers "which
    // ROW", a different question -- so routing on it meant the router
    // never delivered a press to either. The scrollbar had been working
    // only because a press on it also lands inside no row and was
    // reaching uui_table_press() by way of a hit on a NEIGHBOURING row;
    // a header click landed on nothing at all and was silently dropped,
    // which is why clicking a column title appeared to do nothing.
    //
    // Same distinction uui_listbox's lb_ops_bounds() already draws, and
    // for the same reason.
    const struct uui_table *t = (const struct uui_table *)w;
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

static int tb_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_table_key((struct uui_table *)w, key);
}

static int tb_ops_accepts_focus(const void *w) { (void)w; return 1; }
static void tb_ops_set_focused(void *w, int focused) {
    ((struct uui_table *)w)->focused = focused;
}

static int tb_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_table *t = (struct uui_table *)w;
    // The scrollbar outranks the rows: uui_table_hit() excludes the bar
    // column, so a press there has to be offered to the bar first or it
    // reaches nothing at all.
    if (uui_table_press(t, cx, cy)) return 1;
    return uui_table_click(t, cx, cy);
}

static int tb_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_table *t = (struct uui_table *)w;
    if (buttons & 1) return uui_table_drag(t, cx, cy);
    return uui_table_hover(t, cx, cy);
}

static int tb_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    uui_table_drag_end((struct uui_table *)w);
    return 0;
}

static int tb_ops_wheel(void *w, int notches) {
    return uui_table_wheel((struct uui_table *)w, notches);
}

// The rows' pitch, the header's height, each column's x and width, and
// the selection -- what a test needs to click a header or a row
// (ui/uui_describe.h).
static void tb_ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_table *t = (const struct uui_table *)w;
    uui_describe_int(d, "row_h", uui_table_row_h(t));
    uui_describe_int(d, "header_h", uui_table_header_h(t));
    for (int c = 0; c < t->col_count; c++) {
        int cx, cw;
        uui_table_column_rect(t, c, &cx, &cw);
        uui_describe_rect_i(d, "col", c, cx, t->y, cw, t->h);
    }
    uui_describe_int(d, "selected", t->selected);
}

static void tb_ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_table *s = (const struct uui_table *)w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

const struct uui_widget_ops uui_table_ops = {
    .natural_size = tb_ops_natural_size,
    .set_geometry = tb_ops_set_geometry,
    .draw = tb_ops_draw,
    .hit = tb_ops_hit,
    .key = tb_ops_key,
    .accepts_focus = tb_ops_accepts_focus,
    .set_focused   = tb_ops_set_focused,
    .press = tb_ops_press,
    .motion = tb_ops_motion,
    .release = tb_ops_release,
    .wheel = tb_ops_wheel,
    .describe = tb_ops_describe,
    .bounds = tb_ops_bounds,
};
