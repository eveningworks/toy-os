#ifndef UUI_TABLE_H
#define UUI_TABLE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"
#include "ui/uui_seek.h"

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

// Also called by TYPE-AHEAD, which is off the paint path and asks for
// rows that are not visible -- so a cell fn must write only into `out`
// and must not assume it is being drawn.

// --- sorting ----------------------------------------------------------
//
// THE SPLIT, and it is the one every real toolkit makes: the WIDGET
// owns the ordering, the APP owns the comparison.
//
//   * Win32's ListView sends LVN_COLUMNCLICK and the app calls
//     ListView_SortItems() with a comparator; the control permutes its
//     own items.
//   * Qt's QSortFilterProxyModel keeps a row mapping and compares
//     through lessThan(), which the app overrides.
//   * GTK's GtkTreeSortable takes a sort function per column.
//
// None of them sort the DISPLAYED TEXT, and that is the part worth
// copying rather than the API shape: this table's cells are formatted
// strings, so a text sort puts "10" before "9" and orders "4 KB"
// against "1 MB" meaninglessly -- three of Task Manager's five columns
// would be wrong. Qt's default only works because its models hand back
// typed values. Comparing has to happen on the app's real data, which
// only the app can reach.
//
// So an app supplies this and gets everything else -- the clickable
// header, the arrow, click-again-to-reverse, and keyboard motion in the
// sorted order -- without writing any of it.
//
// Returns <0, 0 or >0 for a ASCENDING before/equal/after b, exactly
// like C's qsort comparator. `row_a`/`row_b` are the app's OWN row
// indices (the same ones `cell` is called with), never view positions.
typedef int (*uui_table_cmp_fn)(void *ctx, int row_a, int row_b, int col);

// A row's own background, or 0 for "the ordinary one". Lets an app mark
// a SET of rows without the table knowing what the mark means -- the
// same split as `compare`: the widget draws, the app decides. Selection
// and hover still outrank it, because those say where the pointer and
// the keyboard are and a mark does not move.
//
// The file manager's marked files are the caller; a task manager
// tinting a stopped process would be the second.
typedef uint32_t (*uui_table_tint_fn)(void *ctx, int row);

// The permutation is a fixed array because Toykit has no allocator.
// Past this many rows the table shows the first UUI_TABLE_MAX_ROWS in
// sorted order and the rest unsorted after them, rather than silently
// dropping any -- see uui_table.c. Task Manager's ceiling is
// SYS_PROC_MAX (64).
#define UUI_TABLE_MAX_ROWS 256

#define UUI_TABLE_UNSORTED (-1)

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

    // Sorting. `compare` NULL means this table does not sort: the
    // header is inert and no arrow is drawn, so an app opts in purely
    // by supplying a comparator.
    uui_table_cmp_fn compare;
    uui_table_tint_fn tint;  // NULL = no row ever tinted
    int sort_col;   // UUI_TABLE_UNSORTED, or a column index
    int sort_dir;   // 1 ascending, -1 descending

    // view row -> app row. Rebuilt by uui_table_set_rows() and by
    // uui_table_set_sort(), which is why an app that already calls
    // set_rows() after refreshing its data needs no other hook.
    // `order_rows` is what it was built for, so a row_count that
    // changed without a rebuild is detectable rather than silently
    // indexing stale positions.
    int order[UUI_TABLE_MAX_ROWS];
    int order_rows;

    // A table with NO header: no column titles, no sort arrow, no
    // clickable header row, and the rows start at the widget's top.
    // Set by uui_table_set_header(). This is LVS_REPORT vs LVS_LIST on
    // Win32's list view -- the same widget, and the difference is
    // columns rather than behaviour. uui_fileview's list mode is the
    // caller: a narrow sidebar of filenames wants no column titles.
    int show_header;

    // TYPE-AHEAD. `seek_col` is which column a typed letter matches,
    // GtkTreeView's search-column -- because column 0 is the identifying
    // one in a file listing and the PID in Task Manager. It defaults to
    // 0 rather than to off: a table that searches an unhelpful column
    // says so the first time anyone types, while one that ignores
    // letters entirely fails silently.
    struct uui_seek seek;
    int seek_col;

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

// Turns sorting on: `compare` is called to order rows, and the header
// becomes clickable. Pass NULL to turn it off again.
void uui_table_set_compare(struct uui_table *t, uui_table_cmp_fn compare);

// See uui_table_tint_fn. NULL turns it off again.
void uui_table_set_tint(struct uui_table *t, uui_table_tint_fn tint);

// Shows or hides the header row. On by default; a table with it off
// still sorts if it has a comparator, it just has nothing to click.
void uui_table_set_header(struct uui_table *t, int show);

// Which column type-ahead matches. A negative column turns the search
// off, for a table whose rows have no name worth typing.
void uui_table_set_seek_col(struct uui_table *t, int col);

// Sorts by `col` in `dir` (1 ascending, -1 descending), or clears the
// sort with col = UUI_TABLE_UNSORTED. Rebuilds the order immediately.
void uui_table_set_sort(struct uui_table *t, int col, int dir);

// The APP's row index for a view position, and the inverse. Every
// public function that names a row -- `selected`, `hovered`,
// uui_table_hit() -- speaks the APP's indices, so an app that sorts is
// otherwise unchanged and a selection SURVIVES a re-sort instead of
// jumping to whatever landed in that slot. These two exist for the
// tests and for an app that needs to reason about screen order.
int  uui_table_source_row(const struct uui_table *t, int view_row);
int  uui_table_view_row(const struct uui_table *t, int source_row);

// Which column's HEADER is at (cx, cy), or -1. Public for the same
// reason uui_table_column_rect() is: a test clicks a header without
// re-deriving its geometry in Python.
int  uui_table_header_hit(const struct uui_table *t, int cx, int cy);

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
