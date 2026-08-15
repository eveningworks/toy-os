// See ui_radio_list.h for the design writeup.
#include "ui_radio_list.h"
#include "ui_primitives.h" // widget_hit()
#include "kapi.h"

#define RADIO_LABEL_GAP 8

// Row/column of item `i`, in the column-major-per-row order the list
// reads in: index 0 is top-left, index 1 is the next column along the
// same row. Factored out so draw() and hit() literally cannot disagree
// about where an item is -- the bug ui_checkbox.h's shared-width helper
// exists to prevent, one dimension up.
static void item_cell(const struct ui_radio_list *list, int i, int *out_col, int *out_row) {
    int cols = list->cols > 0 ? list->cols : 1;
    *out_col = i % cols;
    *out_row = i / cols;
}

static int row_count(const struct ui_radio_list *list) {
    int cols = list->cols > 0 ? list->cols : 1;
    return (list->count + cols - 1) / cols; // ceiling, so a partial last row still counts
}

void ui_radio_list_natural_size(const struct ui_radio_list *list, int *out_w, int *out_h) {
    int cols = list->cols > 0 ? list->cols : 1;
    if (cols > list->count && list->count > 0) cols = list->count; // don't reserve empty columns
    if (out_w) *out_w = cols * list->col_w;
    if (out_h) *out_h = row_count(list) * list->row_h;
}

void ui_radio_list_draw(const struct ui_radio_list *list, int x, int y, int selected,
                         int hovered, uint32_t bg, uint32_t fg, uint32_t accent) {
    int text_h = gfx_char_h();
    for (int i = 0; i < list->count; i++) {
        int col, row;
        item_cell(list, i, &col, &row);
        int ix = x + col * list->col_w;
        int iy = y + row * list->row_h;

        // The whole row washes, because the whole row is the hit area
        // (ui_radio_list_hit below). Through ui_state_bg() rather than a
        // hand-picked tint, so it darkens on a light theme instead of
        // lightening into invisibility -- docs/gui-guidelines.md.
        uint32_t row_bg = bg;
        if (i == hovered) {
            row_bg = ui_state_bg(bg, UI_STATE_HOVER);
            gfx_fill_rect(ix, iy, list->col_w, list->row_h, row_bg);
        }

        // Marker: an outlined box, filled with `accent` when active.
        // A square rather than a circle because gfx has no circle
        // primitive and a hand-rasterized one at 14px reads as a lumpy
        // blob -- the fill/outline distinction carries the meaning.
        int m = list->marker_size;
        int marker_y = iy + (text_h - m) / 2;
        gfx_draw_rect(ix, marker_y, m, m, fg);
        if (i == selected) {
            int inset = m / 4 > 0 ? m / 4 : 1;
            gfx_fill_rect(ix + inset, marker_y + inset, m - 2 * inset, m - 2 * inset, accent);
        }

        if (list->options && list->options[i]) {
            // Clipped to what's left of the column: a long option label
            // would otherwise be drawn straight over the next column.
            gfx_draw_string_clipped(ix + m + RADIO_LABEL_GAP, iy,
                                     list->col_w - m - RADIO_LABEL_GAP,
                                     list->options[i], fg, row_bg);
        }
    }
}

int ui_radio_list_hit(const struct ui_radio_list *list, int x, int y, int px, int py) {
    for (int i = 0; i < list->count; i++) {
        int col, row;
        item_cell(list, i, &col, &row);
        int ix = x + col * list->col_w;
        int iy = y + row * list->row_h;
        // The full row rect, not the marker alone -- see the header.
        if (widget_hit(ix, iy, list->col_w, list->row_h, px, py)) return i;
    }
    return -1;
}
