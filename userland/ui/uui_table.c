// table -- rows in columns, with a header. See ui/uui_table.h.
#include "ui/uui_table.h"
#include "ui/uui_widget.h"
#include "lib/string.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// Left/right inset inside a column. Hoisted so natural_size() reserves
// exactly what the draw uses -- the "one geometry, shared" rule the
// scrollbar already follows.
#define UUI_TABLE_PAD_X 4

// Longest cell text this widget will render. A cell is one column wide
// and gets clipped anyway, so this bounds the app's formatting rather
// than the layout.
#define UUI_TABLE_CELL_MAX 96

void uui_table_init(struct uui_table *t, int x, int y, int w, int h,
                     const struct uui_table_column *cols, int col_count,
                     uui_table_cell_fn cell, void *ctx) {
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->cols = cols;
    t->col_count = col_count;
    t->row_count = 0;
    t->cell = cell;
    t->ctx = ctx;
    t->selected = -1;
    t->hovered = -1;
    t->top = 0;
    t->row_h = 0; // derive from the font
    t->bar_w = 8;
    t->thumb_grab = -1;

    t->bg = ugfx_rgb(255, 255, 255);
    t->fg = ugfx_rgb(20, 20, 20);
    t->sel_bg = ugfx_rgb(205, 220, 240);
    t->sel_fg = ugfx_rgb(20, 20, 20);
    t->head_bg = ugfx_rgb(225, 225, 230);
    t->head_fg = ugfx_rgb(40, 40, 40);
    t->grid = ugfx_rgb(205, 205, 210);
    t->track_bg = ugfx_rgb(225, 225, 230);
    t->thumb_bg = ugfx_rgb(150, 155, 165);
}

int uui_table_row_h(const struct uui_table *t) {
    return t->row_h > 0 ? t->row_h : ugfx_char_h() + 4;
}

int uui_table_header_h(const struct uui_table *t) {
    return uui_table_row_h(t);
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

void uui_table_set_rows(struct uui_table *t, int row_count) {
    t->row_count = row_count;
    // Both of these move on every refresh in a task manager: processes
    // come and go under a live view. Clamping HERE is why the app never
    // has to think about it.
    if (t->selected >= row_count) t->selected = row_count > 0 ? row_count - 1 : -1;
    if (t->hovered >= row_count) t->hovered = -1;
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

    int cw = ugfx_char_w();
    int fixed = 0, stretch = 0;
    for (int i = 0; i < t->col_count; i++) {
        if (t->cols[i].width_chars > 0) fixed += t->cols[i].width_chars * cw;
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
            width = t->cols[i].width_chars * cw;
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

void uui_table_draw(struct ugfx_surface *s, const struct uui_table *t) {
    int rh = uui_table_row_h(t);
    int hh = uui_table_header_h(t);
    int vis = uui_table_visible_rows(t);
    int bar = uui_table_scrollbar_visible(t) ? t->bar_w : 0;

    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, t->bg);

    // --- header ---
    ugfx_fill_rect(s, t->x, t->y, t->w, hh, t->head_bg);
    for (int c = 0; c < t->col_count; c++) {
        int cx, cw;
        uui_table_column_rect(t, c, &cx, &cw);
        int avail = cw - UUI_TABLE_PAD_X * 2;
        if (avail <= 0) continue;

        int tx = cx + UUI_TABLE_PAD_X;
        const char *title = t->cols[c].title ? t->cols[c].title : "";
        if (t->cols[c].align == UUI_TALIGN_RIGHT) {
            int tw = ugfx_text_width(title);
            if (tw < avail) tx = cx + cw - UUI_TABLE_PAD_X - tw;
        }
        ugfx_draw_string_clipped(s, tx, t->y + (hh - ugfx_char_h()) / 2,
                                  avail, title, t->head_fg, t->head_bg);
        // Column separator, header only -- a full grid turns a dense
        // table into graph paper, and every desktop table draws the
        // header rule and leaves the body clean.
        if (c > 0) ugfx_fill_rect(s, cx, t->y, 1, hh, t->grid);
    }
    ugfx_fill_rect(s, t->x, t->y + hh - 1, t->w, 1, t->grid);

    // --- rows ---
    for (int i = 0; i < vis; i++) {
        int idx = t->top + i;
        if (idx >= t->row_count) break;
        int ry = t->y + hh + i * rh;

        uint32_t rbg = t->bg, rfg = t->fg;
        if (idx == t->selected) { rbg = t->sel_bg; rfg = t->sel_fg; }
        else if (idx == t->hovered) { rbg = uui_state_bg(t->bg, UUI_STATE_HOVER); }

        if (rbg != t->bg) ugfx_fill_rect(s, t->x, ry, t->w - bar, rh, rbg);
        for (int c = 0; c < t->col_count; c++) draw_cell(s, t, c, idx, ry, rfg, rbg);
    }

    if (bar) {
        // Beside the ROWS, not the header -- a bar that started at the
        // top would let the thumb sit next to column titles it cannot
        // scroll.
        uui_scrollbar_draw(s, t->x + t->w - bar, t->y + hh, bar, t->h - hh,
                            t->row_count, vis, t->row_count - vis - t->top,
                            t->track_bg, t->thumb_bg, 0);
    }
}

void uui_table_natural_size(const struct uui_table *t, int *out_w, int *out_h) {
    if (out_w) {
        int cw = ugfx_char_w(), total = 0;
        for (int i = 0; i < t->col_count; i++) {
            int chars = t->cols[i].width_chars;
            if (chars <= 0) {
                // A stretch column has no width of its own; ask for
                // enough to show its title, and let the layout give
                // more.
                int tw = ugfx_text_width(t->cols[i].title ? t->cols[i].title : "");
                total += tw + UUI_TABLE_PAD_X * 2;
            } else {
                total += chars * cw;
            }
        }
        *out_w = total + t->bar_w;
    }
    // Header plus a few rows: a preferred MINIMUM, not the whole table.
    // Asking for every row would make a window with 60 processes in it
    // taller than the screen.
    if (out_h) *out_h = uui_table_header_h(t) + uui_table_row_h(t) * 6;
}

int uui_table_hit(const struct uui_table *t, int cx, int cy) {
    int bar = uui_table_scrollbar_visible(t) ? t->bar_w : 0;
    int hh = uui_table_header_h(t);
    if (!uui_hit(t->x, t->y + hh, t->w - bar, t->h - hh, cx, cy)) return -1;
    int rh = uui_table_row_h(t);
    int idx = t->top + (cy - t->y - hh) / rh;
    if (idx < 0 || idx >= t->row_count) return -1;
    return idx;
}

int uui_table_hover(struct uui_table *t, int cx, int cy) {
    int idx = uui_table_hit(t, cx, cy);
    if (idx == t->hovered) return 0;
    t->hovered = idx;
    return 1;
}

int uui_table_click(struct uui_table *t, int cx, int cy) {
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
    if (t->selected < t->top) t->top = t->selected;
    else if (t->selected >= t->top + vis) t->top = t->selected - vis + 1;
    table_clamp(t);
}

int uui_table_key(struct uui_table *t, int key) {
    if (t->row_count <= 0) return 0;
    int before = t->selected;
    int vis = uui_table_visible_rows(t);

    if (key == KEY_ARROW_UP)        { if (t->selected > 0) t->selected--; }
    else if (key == KEY_ARROW_DOWN) { if (t->selected < t->row_count - 1) t->selected++; }
    else if (key == KEY_HOME)       { t->selected = 0; }
    else if (key == KEY_END)        { t->selected = t->row_count - 1; }
    else if (key == KEY_PAGE_UP)    { t->selected -= vis;
                                       if (t->selected < 0) t->selected = 0; }
    else if (key == KEY_PAGE_DOWN)  { t->selected += vis;
                                       if (t->selected >= t->row_count) t->selected = t->row_count - 1; }
    else return 0;

    if (t->selected < 0) t->selected = 0;
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
    return uui_table_hit((const struct uui_table *)w, cx, cy) >= 0;
}

static int tb_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_table_key((struct uui_table *)w, key);
}

static int tb_ops_accepts_focus(const void *w) { (void)w; return 1; }

static int tb_ops_press(void *w, int cx, int cy) {
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

const struct uui_widget_ops uui_table_ops = {
    .natural_size = tb_ops_natural_size,
    .set_geometry = tb_ops_set_geometry,
    .draw = tb_ops_draw,
    .hit = tb_ops_hit,
    .key = tb_ops_key,
    .accepts_focus = tb_ops_accepts_focus,
    .press = tb_ops_press,
    .motion = tb_ops_motion,
    .release = tb_ops_release,
    .wheel = tb_ops_wheel,
};
