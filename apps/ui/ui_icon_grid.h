#ifndef UI_ICON_GRID_H
#define UI_ICON_GRID_H

// A small reusable icon-grid geometry + drag-to-reposition helper.
// First caller: the desktop icon grid (apps/wm/desktop.c); built as a
// standalone widget (not desktop.c-local) so a future file manager's
// icon view (docs/roadmap.md Milestone 13) can reuse the same cell
// math and drag session instead of re-deriving it. Same split as the
// rest of apps/ui/ (see ui_button.h's top comment): this module only
// knows grid geometry (cell <-> pixel) and a drag session (which item,
// where it was grabbed) -- callers own their own per-item {col,row}
// storage, labels, and glyphs, and decide their own persistence (or
// none).
//
// The drag session mirrors apps/wm/wm_input.c's window-dragging shape
// exactly (its `dragging`/`drag_off_x`/`drag_off_y`): mouse-down arms
// it with the pixel offset from the item's top-left to the grab point
// (so the item tracks the cursor instead of snapping its corner under
// it), a per-tick update reports the live snapped-to-grid preview
// cell, and mouse-up ends the session. It's just that shape factored
// out so a second caller doesn't have to hand-roll it again.

struct icon_grid {
    int origin_x, origin_y; // top-left pixel of cell (0, 0)
    int cell_w, cell_h;     // full cell footprint (icon box + label + gaps)
    int cols;               // column count the grid wraps at
};

// Top-left pixel of the icon box at (col, row).
void icon_grid_cell_rect(const struct icon_grid *grid, int col, int row,
                          int *out_x, int *out_y);

// Nearest valid cell to a pixel position -- clamps col to [0, cols-1]
// and row to >= 0, so this always returns a cell, never "no match".
void icon_grid_nearest_cell(const struct icon_grid *grid, int px, int py,
                             int *out_col, int *out_row);

struct icon_drag {
    int active;
    int index;             // caller's own item index, opaque to this module
    int grab_off_x, grab_off_y;
};

// Arms a drag on mouse-down: (mx, my) is the grab point, (item_x,
// item_y) the item's current top-left pixel position (from
// icon_grid_cell_rect() against its current cell).
void icon_drag_start(struct icon_drag *drag, int index,
                      int mx, int my, int item_x, int item_y);

// Live preview cell under the cursor -- call every tick the drag is
// active, using the same grid passed to icon_grid_cell_rect() above.
void icon_drag_update(const struct icon_drag *drag, const struct icon_grid *grid,
                       int mx, int my, int *out_col, int *out_row);

void icon_drag_end(struct icon_drag *drag);

#endif
