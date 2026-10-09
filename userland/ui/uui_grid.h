#ifndef UUI_GRID_H
#define UUI_GRID_H

// uui_grid -- a GRID OF EQUAL CELLS the caller paints, one selected: a
// character map's glyphs (the Character Map), an icon picker, a board.
// GtkFlowBox and WinUI's GridView in shape: the widget owns the layout,
// the scrolling, the selection and the keys; the caller owns the cell.
//
// THE CELLS FILL THE WIDTH: as many columns as `cell_w` fits, each then
// widened to share the leftover, so a resize reflows rather than leaving
// a ragged right edge. Rows scroll, with the overlay scrollbar.
//
// THE SELECTION IS DRAWN BY THE WIDGET (a solid accent cell, the design
// language's latched state) and the caller is told it is painting a
// selected cell, so it can draw its content in the accent's text colour.
// A click selects; a DOUBLE click or Enter ACTIVATES, parked for
// uui_grid_take(). Arrows move by one cell and one row, Page Up/Down by
// a screenful, Home/End to the ends.
#include <stdint.h>
#include "ui/uui_widget.h"

enum { UUI_GRID_SELECTED = 1, UUI_GRID_HOT = 2, UUI_GRID_FOCUSED = 4 };

// Paint cell `index` in the rect given; `state` is UUI_GRID_* bits. The
// ground is already painted (and the selection's fill). A LOOKUP, like
// every draw callback: no file read, no decode.
typedef void (*uui_grid_cell_fn)(struct ugfx_surface *s, void *ctx, int index,
                                 int x, int y, int w, int h, int state);

struct uui_grid {
    int x, y, w, h;
    int count;
    int cell_w, cell_h;         // the smallest cell; columns are widened from it
    int selected;               // -1 for none
    int scroll;                 // pixels
    uui_grid_cell_fn cell;
    void *ctx;
    uint32_t bg;
    // OWNED
    int hot, armed, activated, focused;
    int bar_hot, bar_drag, bar_grab;
    int last_click;
    uint64_t last_click_ns;
};

void uui_grid_init(struct uui_grid *g, int cell_w, int cell_h);
void uui_grid_set(struct uui_grid *g, int count, uui_grid_cell_fn cell, void *ctx);
// Selects `index` (or -1) and scrolls it into view.
void uui_grid_select(struct uui_grid *g, int index);
// The cell a double-click or Enter activated since the last call, or -1.
int  uui_grid_take(struct uui_grid *g);
int  uui_grid_columns(const struct uui_grid *g);
// Where cell `index` is on screen; 0 when it is scrolled out of view.
int  uui_grid_cell_rect(const struct uui_grid *g, int index, int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_grid_ops;

#endif
