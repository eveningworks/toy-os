// radio list. Split out of uwidgets.c -- see ui/uui_radio_list.h.
#include "ui/uui_radio_list.h"

// ---------------------------------------------------------------------
// radio list
// ---------------------------------------------------------------------

static int radio_rows(const struct uui_radio_list *l) {
    int cols = l->cols > 0 ? l->cols : 1;
    return (l->count + cols - 1) / cols;
}

void uui_radio_list_natural_size(const struct uui_radio_list *l, int *out_w, int *out_h) {
    int cols = l->cols > 0 ? l->cols : 1;
    if (out_w) *out_w = cols * l->col_w;
    if (out_h) *out_h = radio_rows(l) * l->row_h;
}

static void radio_cell(const struct uui_radio_list *l, int i, int x, int y, int *cx, int *cy) {
    int cols = l->cols > 0 ? l->cols : 1;
    *cx = x + (i % cols) * l->col_w;
    *cy = y + (i / cols) * l->row_h;
}

void uui_radio_list_set_geometry(struct uui_radio_list *l, int x, int y) {
    l->x = x;
    l->y = y;
    uui_radio_list_natural_size(l, &l->w, &l->h);
}

void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l,
                          int selected, int hovered,
                          uint32_t bg, uint32_t fg) {
    int x = l->x, y = l->y;
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

int uui_radio_list_hit(const struct uui_radio_list *l, int cx, int cy) {
    int x = l->x, y = l->y, px = cx, py = cy;
    for (int i = 0; i < l->count; i++) {
        int cx, cy;
        radio_cell(l, i, x, y, &cx, &cy);
        if (uui_hit(cx, cy, l->col_w, l->row_h, px, py)) return i;
    }
    return -1;
}
