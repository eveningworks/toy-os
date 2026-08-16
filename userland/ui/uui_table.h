#ifndef UUI_TABLE_H
#define UUI_TABLE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"

// --- table: rows in columns, with a header ---------------------------
//
// The widget Task Manager is built from, and the first one here with
// more than one value per row. A listbox with columns bolted on would
// have been the smaller change and the wrong one: the column widths,
// the header, the per-cell alignment and the horizontal clipping all
// belong together, and `uui_listbox` is already the right thing for a
// list of strings.
//
// **Rows are PULLED, not stored.** The table holds no data at all: it
// asks the app for one cell at a time through `cell`. That is what lets
// Task Manager re-read the process table on every refresh without
// allocating, copying or invalidating anything -- there is no allocator
// in Toykit, and a widget that owned rows would need one. It also means
// the app's data can change under the table between frames and nothing
// goes stale, because nothing was cached.
//
// **Sizing is derived, so the table follows its window.** Column widths
// are in CHARACTERS (font-derived, per docs/gui-guidelines.md), and a
// column may instead declare width 0 to absorb whatever is left over.
// Visible rows come from the height it is given. So a resizable window
// reflows for free: give the table a new geometry and it shows more
// rows and a wider stretch column, with no app arithmetic.

// Cell text for (row, col), written into `out`. The app formats
// whatever it likes; the table clips it to the column.
//
// Called during DRAW and only for visible cells, so an app may format
// into a shared scratch buffer.
typedef void (*uui_table_cell_fn)(void *ctx, int row, int col,
                                   char *out, int cap);

#define UUI_TALIGN_LEFT  0
#define UUI_TALIGN_RIGHT 1 // numbers -- a right-aligned column of sizes
                            // is readable in a way a ragged one is not

struct uui_table_column {
    const char *title;
    int width_chars; // 0 = STRETCH: share out whatever the fixed
                      // columns did not use. More than one stretch
                      // column splits the remainder evenly.
    int align;       // UUI_TALIGN_*
};

struct uui_table {
    int x, y, w, h;

    const struct uui_table_column *cols; // caller-owned
    int col_count;
    int row_count;

    uui_table_cell_fn cell;
    void *ctx;

    int selected;   // row index, or -1
    int hovered;    // OWNED -- driven by uui_table_hover()
    int top;        // first visible row; OWNED
    int row_h;      // 0 = derive from the font
    int bar_w;

    // Live thumb-drag state: -1 when none, else the grab offset WITHIN
    // the thumb. OWNED. Passing 0 here instead of the real offset is
    // what made a ring-3 scrollbar grabbable only by its top edge once
    // already -- see docs/gui-guidelines.md's scrollbar section.
    int thumb_grab;

    uint32_t bg, fg, sel_bg, sel_fg;
    uint32_t head_bg, head_fg, grid;
    uint32_t track_bg, thumb_bg;
};

void uui_table_init(struct uui_table *t, int x, int y, int w, int h,
                     const struct uui_table_column *cols, int col_count,
                     uui_table_cell_fn cell, void *ctx);

// The row count changed (the app re-read its data). Clamps `top` and
// `selected` so neither points past the end -- which is the whole
// reason this is a function rather than an assignment.
void uui_table_set_rows(struct uui_table *t, int row_count);

int  uui_table_row_h(const struct uui_table *t);
int  uui_table_header_h(const struct uui_table *t);
int  uui_table_visible_rows(const struct uui_table *t);
int  uui_table_scrollbar_visible(const struct uui_table *t);

// Where column `col` starts and how wide it is, both content-relative.
// Public because a test asserts on it rather than re-deriving it in
// Python -- the lesson four tools here have already paid for.
void uui_table_column_rect(const struct uui_table *t, int col,
                            int *out_x, int *out_w);

void uui_table_draw(struct ugfx_surface *s, const struct uui_table *t);
void uui_table_natural_size(const struct uui_table *t, int *out_w, int *out_h);

// Row index at (cx, cy), or -1 outside / on the header / on the bar.
int  uui_table_hit(const struct uui_table *t, int cx, int cy);
int  uui_table_hover(struct uui_table *t, int cx, int cy);  // 1 if changed
int  uui_table_click(struct uui_table *t, int cx, int cy);  // selects; 1 if changed
int  uui_table_wheel(struct uui_table *t, int notches);     // 1 if scrolled

int  uui_table_press(struct uui_table *t, int cx, int cy);
int  uui_table_drag(struct uui_table *t, int cx, int cy);
void uui_table_drag_end(struct uui_table *t);
int  uui_table_key(struct uui_table *t, int key);           // arrows/home/end/page

struct uui_widget_ops;
extern const struct uui_widget_ops uui_table_ops;

#endif
