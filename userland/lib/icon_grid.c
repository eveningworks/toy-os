// See lib/icon_grid.h for the design writeup.
#include "lib/icon_grid.h"

void icon_grid_cell_rect(const struct icon_grid *grid, int col, int row,
                          int *out_x, int *out_y) {
    *out_x = grid->origin_x + col * grid->cell_w;
    *out_y = grid->origin_y + row * grid->cell_h;
}

void icon_grid_nearest_cell(const struct icon_grid *grid, int px, int py,
                             int *out_col, int *out_row) {
    int col = (px - grid->origin_x + grid->cell_w / 2) / grid->cell_w;
    int row = (py - grid->origin_y + grid->cell_h / 2) / grid->cell_h;
    if (col < 0) col = 0;
    if (col >= grid->cols) col = grid->cols - 1;
    if (row < 0) row = 0;
    *out_col = col;
    *out_row = row;
}

void icon_drag_start(struct icon_drag *drag, int index,
                      int mx, int my, int item_x, int item_y) {
    drag->active = 1;
    drag->index = index;
    drag->grab_off_x = mx - item_x;
    drag->grab_off_y = my - item_y;
}

void icon_drag_update(const struct icon_drag *drag, const struct icon_grid *grid,
                       int mx, int my, int *out_col, int *out_row) {
    icon_grid_nearest_cell(grid, mx - drag->grab_off_x, my - drag->grab_off_y, out_col, out_row);
}

void icon_drag_end(struct icon_drag *drag) {
    drag->active = 0;
    drag->index = -1;
}
