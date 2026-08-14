// See uwidgets.h. Ported from apps/ui/, adapted to draw into a client's
// own surface rather than the framebuffer.
#include "uwidgets.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

#define CARET_W 2

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

// ---------------------------------------------------------------------
// scrollbar
// ---------------------------------------------------------------------

// THE shared geometry. Thumb size and position depend only on the line
// counts, never on x -- computing it once here is what stops draw(),
// hit() and the drag maths disagreeing about where the thumb is.
static void sb_geometry(int y, int h, int total_lines, int visible_rows, int scroll_offset,
                         int *out_thumb_y, int *out_thumb_h, int *out_max_scroll) {
    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;

    int thumb_h = (total_lines > 0) ? h * visible_rows / total_lines : h;
    if (thumb_h < UUI_SCROLLBAR_MIN_THUMB_H) thumb_h = UUI_SCROLLBAR_MIN_THUMB_H;
    if (thumb_h > h) thumb_h = h;

    int track_range = h - thumb_h;
    int thumb_y = y;
    if (max_scroll > 0 && track_range > 0) {
        // offset 0 (pinned to newest) -> thumb at the BOTTOM;
        // offset max_scroll (oldest) -> thumb at the top.
        thumb_y = y + track_range - track_range * scroll_offset / max_scroll;
    }

    *out_thumb_y = thumb_y;
    *out_thumb_h = thumb_h;
    *out_max_scroll = max_scroll;
}

void uui_scrollbar_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                         int total_lines, int visible_rows, int scroll_offset,
                         uint32_t track_bg, uint32_t thumb_bg) {
    ugfx_fill_rect(s, x, y, w, h, track_bg);
    if (total_lines <= visible_rows) return; // it all fits -- no thumb

    int ty, th, ms;
    sb_geometry(y, h, total_lines, visible_rows, scroll_offset, &ty, &th, &ms);
    ugfx_fill_rect(s, x, ty, w, th, thumb_bg);
}

enum uui_scrollbar_zone uui_scrollbar_hit(int x, int y, int w, int h,
                                           int total_lines, int visible_rows,
                                           int scroll_offset, int px, int py) {
    if (!uui_hit(x, y, w, h, px, py)) return UUI_SB_NONE;
    if (total_lines <= visible_rows) return UUI_SB_NONE;

    int ty, th, ms;
    sb_geometry(y, h, total_lines, visible_rows, scroll_offset, &ty, &th, &ms);
    if (py < ty) return UUI_SB_ABOVE;
    if (py >= ty + th) return UUI_SB_BELOW;
    return UUI_SB_THUMB;
}

void uui_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                               int scroll_offset, int *out_thumb_y, int *out_thumb_h) {
    int ms;
    sb_geometry(y, h, total_lines, visible_rows, scroll_offset, out_thumb_y, out_thumb_h, &ms);
}

int uui_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                   int py, int grab_offset_in_thumb) {
    int ty, th, max_scroll;
    // The 0 here is arbitrary: only thumb_h and max_scroll are used, and
    // the drag itself determines the new position.
    sb_geometry(y, h, total_lines, visible_rows, 0, &ty, &th, &max_scroll);
    if (max_scroll <= 0) return 0;

    int track_range = h - th;
    if (track_range <= 0) return 0;

    int new_y = py - grab_offset_in_thumb;
    if (new_y < y) new_y = y;
    if (new_y > y + track_range) new_y = y + track_range;

    int offset = max_scroll - (new_y - y) * max_scroll / track_range;
    if (offset < 0) offset = 0;
    if (offset > max_scroll) offset = max_scroll;
    return offset;
}

// ---------------------------------------------------------------------
// text field
// ---------------------------------------------------------------------

void uui_field_init(struct uui_field *f, const char *initial) {
    int i = 0;
    if (initial) {
        while (initial[i] && i < UUI_FIELD_MAX - 1) { f->buf[i] = initial[i]; i++; }
    }
    f->buf[i] = '\0';
    f->len = i;
    f->cursor = i;
    f->active = 0;
}

void uui_field_set_active(struct uui_field *f, int active) { f->active = active ? 1 : 0; }

static void field_insert(struct uui_field *f, char c) {
    if (f->len >= UUI_FIELD_MAX - 1) return;
    for (int i = f->len; i > f->cursor; i--) f->buf[i] = f->buf[i - 1];
    f->buf[f->cursor] = c;
    f->len++;
    f->cursor++;
    f->buf[f->len] = '\0';
}

static void field_delete(struct uui_field *f) {
    if (f->cursor >= f->len) return;
    for (int i = f->cursor; i < f->len - 1; i++) f->buf[i] = f->buf[i + 1];
    f->len--;
    f->buf[f->len] = '\0';
}

int uui_field_key(struct uui_field *f, int key) {
    if (!f->active) return 0;

    if (key == '\b') {
        if (f->cursor > 0) { f->cursor--; field_delete(f); }
    } else if (key == KEY_DELETE) {
        field_delete(f);
    } else if (key == KEY_ARROW_LEFT) {
        if (f->cursor > 0) f->cursor--;
    } else if (key == KEY_ARROW_RIGHT) {
        if (f->cursor < f->len) f->cursor++;
    } else if (key == KEY_HOME) {
        f->cursor = 0;
    } else if (key == KEY_END) {
        f->cursor = f->len;
    } else if (key >= 32 && key < 127) {
        field_insert(f, (char)key);
    } else {
        // Notably Enter: committing a field is the caller's decision.
        return 0;
    }
    return 1;
}

void uui_field_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                     const struct uui_field *f,
                     uint32_t bg, uint32_t fg, uint32_t border) {
    ugfx_fill_rect(s, x, y, w, h, bg);
    ugfx_draw_rect(s, x, y, w, h, border);

    int pad = 4;
    int char_w = ugfx_char_w();
    int ty = y + (h - ugfx_char_h()) / 2;

    // Horizontal windowing: how many characters fit, and how far right
    // the window has to slide to keep the caret inside it. Without this
    // a long value draws straight through the border -- a bug the
    // kernel widget actually had.
    int visible = char_w > 0 ? (w - 2 * pad) / char_w : 0;
    if (visible < 0) visible = 0;
    int start = 0;
    if (f->active && f->len > visible) {
        start = f->cursor - visible + 1;
        if (start < 0) start = 0;
        int max_start = f->len - visible;
        if (start > max_start) start = max_start;
    }

    char shown[UUI_FIELD_MAX];
    int n = 0;
    for (; n < visible && f->buf[start + n]; n++) shown[n] = f->buf[start + n];
    shown[n] = '\0';
    // Clipped as a safety net, not because the slice is expected to be
    // wrong: if the windowing ever miscomputes, text stops at the edge
    // rather than drawing through the border.
    ugfx_draw_string_clipped(s, x + pad, ty, w - 2 * pad, shown, fg, bg);

    if (f->active) {
        ugfx_fill_rect(s, x + pad + (f->cursor - start) * char_w, ty,
                        CARET_W, ugfx_char_h(), fg);
    }
}

// ---------------------------------------------------------------------
// checkbox
// ---------------------------------------------------------------------

#define CHECKBOX_LABEL_GAP 6

int uui_checkbox_width(int size, const char *label) {
    if (!label) return size;
    return size + CHECKBOX_LABEL_GAP + slen(label) * ugfx_char_w();
}

void uui_checkbox_draw(struct ugfx_surface *s, int x, int y, int size,
                        int checked, int hovered, const char *label,
                        uint32_t bg, uint32_t fg) {
    if (hovered) {
        // The WHOLE clickable area, because that is what the hit test
        // covers -- a highlight smaller than its target misleads about
        // where to click.
        int hw = uui_checkbox_width(size, label);
        int hh = size > ugfx_char_h() ? size : ugfx_char_h();
        bg = uui_state_bg(bg, UUI_STATE_HOVER);
        ugfx_fill_rect(s, x, y, hw, hh, bg);
    }
    ugfx_draw_rect(s, x, y, size, size, fg);
    if (checked) {
        int inset = size / 4 > 0 ? size / 4 : 1;
        ugfx_fill_rect(s, x + inset, y + inset, size - 2 * inset, size - 2 * inset, fg);
    }
    if (label) {
        ugfx_draw_string(s, x + size + CHECKBOX_LABEL_GAP,
                          y + (size - ugfx_char_h()) / 2, label, fg, bg);
    }
}

int uui_checkbox_hit(int x, int y, int size, const char *label, int px, int py) {
    int w = uui_checkbox_width(size, label);
    int row_h = ugfx_char_h();
    int h = size > row_h ? size : row_h;
    return uui_hit(x, y, w, h, px, py);
}

// ---------------------------------------------------------------------
// radio list
// ---------------------------------------------------------------------

static int radio_rows(const struct uui_radio_list *l) {
    int cols = l->cols > 0 ? l->cols : 1;
    return (l->count + cols - 1) / cols;
}

void uui_radio_list_size(const struct uui_radio_list *l, int *out_w, int *out_h) {
    int cols = l->cols > 0 ? l->cols : 1;
    if (out_w) *out_w = cols * l->col_w;
    if (out_h) *out_h = radio_rows(l) * l->row_h;
}

static void radio_cell(const struct uui_radio_list *l, int i, int x, int y, int *cx, int *cy) {
    int cols = l->cols > 0 ? l->cols : 1;
    *cx = x + (i % cols) * l->col_w;
    *cy = y + (i / cols) * l->row_h;
}

void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l,
                          int x, int y, int selected, int hovered,
                          uint32_t bg, uint32_t fg) {
    for (int i = 0; i < l->count; i++) {
        int cx, cy;
        radio_cell(l, i, x, y, &cx, &cy);

        uint32_t row_bg = bg;
        if (i == hovered) {
            row_bg = uui_state_bg(bg, UUI_STATE_HOVER);
            ugfx_fill_rect(s, cx, cy, l->col_w, l->row_h, row_bg);
        }

        int m = l->marker_size;
        int my = cy + (l->row_h - m) / 2;
        ugfx_draw_rect(s, cx, my, m, m, fg);
        if (i == selected) {
            int inset = m / 4 > 0 ? m / 4 : 1;
            ugfx_fill_rect(s, cx + inset, my + inset, m - 2 * inset, m - 2 * inset, fg);
        }
        ugfx_draw_string_clipped(s, cx + m + 6, cy + (l->row_h - ugfx_char_h()) / 2,
                                  l->col_w - m - 8, l->options[i], fg, row_bg);
    }
}

int uui_radio_list_hit(const struct uui_radio_list *l, int x, int y, int px, int py) {
    for (int i = 0; i < l->count; i++) {
        int cx, cy;
        radio_cell(l, i, x, y, &cx, &cy);
        if (uui_hit(cx, cy, l->col_w, l->row_h, px, py)) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------
// listbox
// ---------------------------------------------------------------------

void uui_listbox_init(struct uui_listbox *lb, int x, int y, int w, int h,
                       const char *const *items, int count) {
    lb->x = x; lb->y = y; lb->w = w; lb->h = h;
    lb->items = items;
    lb->count = count;
    lb->selected = count > 0 ? 0 : -1;
    lb->hovered = -1;
    lb->top = 0;
    lb->row_h = 0; // derive from the font
    lb->bar_w = 8;
    lb->bg = ugfx_rgb(255, 255, 255);
    lb->fg = ugfx_rgb(20, 20, 20);
    lb->sel_bg = ugfx_rgb(205, 220, 240);
    lb->sel_fg = ugfx_rgb(20, 20, 20);
    lb->track_bg = ugfx_rgb(225, 225, 230);
    lb->thumb_bg = ugfx_rgb(150, 155, 165);
}

void uui_listbox_set_items(struct uui_listbox *lb, const char *const *items, int count) {
    lb->items = items;
    lb->count = count;
    if (lb->selected >= count) lb->selected = count > 0 ? count - 1 : -1;
    if (lb->top > count) lb->top = 0;
    lb->hovered = -1;
}

int uui_listbox_row_h(const struct uui_listbox *lb) {
    return lb->row_h > 0 ? lb->row_h : ugfx_char_h() + 4;
}

int uui_listbox_visible_rows(const struct uui_listbox *lb) {
    int rh = uui_listbox_row_h(lb);
    int n = rh > 0 ? lb->h / rh : 0;
    return n > 0 ? n : 1;
}

int uui_listbox_scrollbar_visible(const struct uui_listbox *lb) {
    return lb->count > uui_listbox_visible_rows(lb);
}

// Clamps `top` to a range that always fills the box when it can -- a
// list scrolled past its end leaves blank rows below real ones, which
// reads as missing content rather than as the end of the list.
static void listbox_clamp(struct uui_listbox *lb) {
    int vis = uui_listbox_visible_rows(lb);
    int max_top = lb->count > vis ? lb->count - vis : 0;
    if (lb->top > max_top) lb->top = max_top;
    if (lb->top < 0) lb->top = 0;
}

void uui_listbox_draw(struct ugfx_surface *s, const struct uui_listbox *lb) {
    int rh = uui_listbox_row_h(lb);
    int vis = uui_listbox_visible_rows(lb);
    int bar = uui_listbox_scrollbar_visible(lb) ? lb->bar_w : 0;
    int text_w = lb->w - bar;

    ugfx_fill_rect(s, lb->x, lb->y, lb->w, lb->h, lb->bg);

    for (int i = 0; i < vis; i++) {
        int idx = lb->top + i;
        if (idx >= lb->count) break;
        int ry = lb->y + i * rh;

        uint32_t rbg = lb->bg, rfg = lb->fg;
        if (idx == lb->selected) { rbg = lb->sel_bg; rfg = lb->sel_fg; }
        else if (idx == lb->hovered) { rbg = uui_state_bg(lb->bg, UUI_STATE_HOVER); }

        if (rbg != lb->bg) ugfx_fill_rect(s, lb->x, ry, text_w, rh, rbg);
        ugfx_draw_string_clipped(s, lb->x + 4, ry + (rh - ugfx_char_h()) / 2,
                                  text_w - 8, lb->items[idx], rfg, rbg);
    }

    if (bar) {
        uui_scrollbar_draw(s, lb->x + lb->w - bar, lb->y, bar, lb->h,
                            lb->count, vis, lb->count - vis - lb->top,
                            lb->track_bg, lb->thumb_bg);
    }
}

int uui_listbox_hit(const struct uui_listbox *lb, int cx, int cy) {
    int bar = uui_listbox_scrollbar_visible(lb) ? lb->bar_w : 0;
    if (!uui_hit(lb->x, lb->y, lb->w - bar, lb->h, cx, cy)) return -1;
    int rh = uui_listbox_row_h(lb);
    int idx = lb->top + (cy - lb->y) / rh;
    if (idx < 0 || idx >= lb->count) return -1;
    return idx;
}

int uui_listbox_hover(struct uui_listbox *lb, int cx, int cy) {
    int idx = uui_listbox_hit(lb, cx, cy);
    if (idx == lb->hovered) return 0;
    lb->hovered = idx;
    return 1;
}

int uui_listbox_click(struct uui_listbox *lb, int cx, int cy) {
    int idx = uui_listbox_hit(lb, cx, cy);
    if (idx < 0 || idx == lb->selected) return 0;
    lb->selected = idx;
    return 1;
}

int uui_listbox_wheel(struct uui_listbox *lb, int notches) {
    int before = lb->top;
    // Scrolling must NOT change the selection -- a wheel over a list is
    // navigation, not a choice. The kernel version documents the same.
    lb->top -= notches * 3;
    listbox_clamp(lb);
    return lb->top != before;
}

// Keeps the selected row inside the visible window after a keyboard
// move, which is the whole reason arrow keys feel broken without it.
static void listbox_reveal(struct uui_listbox *lb) {
    int vis = uui_listbox_visible_rows(lb);
    if (lb->selected < lb->top) lb->top = lb->selected;
    else if (lb->selected >= lb->top + vis) lb->top = lb->selected - vis + 1;
    listbox_clamp(lb);
}

int uui_listbox_key(struct uui_listbox *lb, int key) {
    if (lb->count <= 0) return 0;
    int before = lb->selected;

    if (key == KEY_ARROW_UP)        { if (lb->selected > 0) lb->selected--; }
    else if (key == KEY_ARROW_DOWN) { if (lb->selected < lb->count - 1) lb->selected++; }
    else if (key == KEY_HOME)       { lb->selected = 0; }
    else if (key == KEY_END)        { lb->selected = lb->count - 1; }
    else if (key == KEY_PAGE_UP)    { lb->selected -= uui_listbox_visible_rows(lb);
                                       if (lb->selected < 0) lb->selected = 0; }
    else if (key == KEY_PAGE_DOWN)  { lb->selected += uui_listbox_visible_rows(lb);
                                       if (lb->selected >= lb->count) lb->selected = lb->count - 1; }
    else return 0;

    listbox_reveal(lb);
    return lb->selected != before;
}

// ---------------------------------------------------------------------
// dropdown -- composes the listbox as its popup
// ---------------------------------------------------------------------

void uui_dropdown_init(struct uui_dropdown *d, int x, int y, int w, int h,
                        const char *const *items, int count) {
    d->x = x; d->y = y; d->w = w; d->h = h;
    d->open = 0;
    d->max_rows = 6;
    d->bg = ugfx_rgb(255, 255, 255);
    d->fg = ugfx_rgb(20, 20, 20);
    d->border = ugfx_rgb(150, 155, 165);

    int rh = ugfx_char_h() + 4;
    int rows = count < d->max_rows ? count : d->max_rows;
    uui_listbox_init(&d->list, x, y + h, w, rows > 0 ? rows * rh : rh, items, count);
}

int uui_dropdown_selected(const struct uui_dropdown *d) { return d->list.selected; }

void uui_dropdown_draw(struct ugfx_surface *s, const struct uui_dropdown *d) {
    ugfx_fill_rect(s, d->x, d->y, d->w, d->h, d->bg);
    ugfx_draw_rect(s, d->x, d->y, d->w, d->h, d->border);

    const char *label = (d->list.selected >= 0 && d->list.selected < d->list.count)
                            ? d->list.items[d->list.selected] : "";
    ugfx_draw_string_clipped(s, d->x + 6, d->y + (d->h - ugfx_char_h()) / 2,
                              d->w - 24, label, d->fg, d->bg);

    // A caret so it reads as a dropdown rather than a text field.
    int cx = d->x + d->w - 14, cy = d->y + d->h / 2 - 2;
    for (int i = 0; i < 5; i++) ugfx_fill_rect(s, cx + i, cy + i, 5 - i * 2 + 4, 1, d->fg);
}

void uui_dropdown_draw_popup(struct ugfx_surface *s, const struct uui_dropdown *d) {
    if (!d->open) return;
    uui_listbox_draw(s, &d->list);
    ugfx_draw_rect(s, d->list.x, d->list.y, d->list.w, d->list.h, d->border);
}

int uui_dropdown_hit(const struct uui_dropdown *d, int cx, int cy) {
    return uui_hit(d->x, d->y, d->w, d->h, cx, cy);
}

int uui_dropdown_click(struct uui_dropdown *d, int cx, int cy) {
    if (uui_dropdown_hit(d, cx, cy)) {
        d->open = !d->open;
        return 1;
    }
    if (d->open) {
        if (uui_hit(d->list.x, d->list.y, d->list.w, d->list.h, cx, cy)) {
            uui_listbox_click(&d->list, cx, cy);
            d->open = 0; // committing closes it
            return 1;
        }
        // A click anywhere else DISMISSES rather than falling through to
        // whatever is underneath -- an open popup owns the next click.
        d->open = 0;
        return 1;
    }
    return 0;
}

int uui_dropdown_key(struct uui_dropdown *d, int key) {
    if (!d->open) {
        if (key == '\n' || key == '\r' || key == ' ' || key == KEY_ARROW_DOWN) {
            d->open = 1;
            return 1;
        }
        return 0;
    }
    if (key == 0x1B) { d->open = 0; return 1; }             // Esc dismisses
    if (key == '\n' || key == '\r') { d->open = 0; return 1; } // Enter commits
    return uui_listbox_key(&d->list, key);
}

// ---------------------------------------------------------------------
// focus ring
// ---------------------------------------------------------------------

void uui_focus_init(struct uui_focus *f, struct uui_focusable *items, int count) {
    f->items = items;
    f->count = count;
    f->current = -1;
}

void uui_focus_set(struct uui_focus *f, int index) {
    if (f->current >= 0 && f->current < f->count) {
        const struct uui_focusable *it = &f->items[f->current];
        if (it->ops->set_focused) it->ops->set_focused(it->widget, 0);
    }
    f->current = (index >= 0 && index < f->count) ? index : -1;
    if (f->current >= 0) {
        const struct uui_focusable *it = &f->items[f->current];
        if (it->ops->set_focused) it->ops->set_focused(it->widget, 1);
    }
}

int uui_focus_next(struct uui_focus *f) {
    if (f->count <= 0) return 0;
    uui_focus_set(f, (f->current + 1) % f->count);
    return 1;
}

int uui_focus_prev(struct uui_focus *f) {
    if (f->count <= 0) return 0;
    uui_focus_set(f, (f->current - 1 + f->count) % f->count);
    return 1;
}

int uui_focus_key(struct uui_focus *f, int key, unsigned mods) {
    if (key == '\t') return (mods & KEY_MOD_SHIFT) ? uui_focus_prev(f) : uui_focus_next(f);
    if (f->current < 0 || f->current >= f->count) return 0;
    const struct uui_focusable *it = &f->items[f->current];
    return it->ops->key ? it->ops->key(it->widget, key) : 0;
}

int uui_focus_click(struct uui_focus *f, int cx, int cy) {
    for (int i = 0; i < f->count; i++) {
        const struct uui_focusable *it = &f->items[i];
        if (it->ops->hit && it->ops->hit(it->widget, cx, cy)) {
            if (i == f->current) return 0;
            uui_focus_set(f, i);
            return 1;
        }
    }
    return 0;
}
