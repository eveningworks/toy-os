#ifndef UUI_TABLE_H
#define UUI_TABLE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_sbar.h"
#include "ui/uui_seek.h"
#include "ui/uui_scrollanim.h"

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

// FADED text on one row, for a row that is present but pending -- a
// file staged by a cut is Explorer's and Dolphin's case, and both fade
// the label rather than tinting the row, because a tint is already
// "marked" here. Returns 1 to fade. NULL = no row ever faded.
typedef int (*uui_table_fade_fn)(void *ctx, int row);

// A small picture before a row's FIRST cell -- a file's type in a
// listing, Explorer's and Dolphin's details view. `px` is the height to
// draw at; return NULL for none. Called during DRAW for visible rows, so
// it must be a CACHE LOOKUP (lib/icon_cache.h's icon_get() is one).
struct uimg;
typedef const struct uimg *(*uui_table_icon_fn)(void *ctx, int row, int px);

// A cell's HEAT, 0..255: how strongly to shade it in the theme's accent
// -- Windows Task Manager's busy CPU and Memory cells. 0 = unshaded.
// The table picks the colour, so it follows the theme; selection still
// outranks it, since a shaded selected row would hide the selection.
typedef int (*uui_table_heat_fn)(void *ctx, int row, int col);

// A cell's METER, 0..1000 per mille, or -1 for none: a thin bar along the
// foot of the cell, under its text -- a folder's share of the folder it
// is in (Windows' drive bars, WinDirStat's column). Drawn in the accent,
// over a track; the selection keeps it, since it is data, not a state.
typedef int (*uui_table_meter_fn)(void *ctx, int row, int col);

// --- groups and a tree --------------------------------------------------
//
// Both are ORDERINGS, so they live beside the sort in `order`: groups
// outermost, then the tree, then the comparator among SIBLINGS -- so a
// sorted tree keeps each child under its parent, which is what
// GtkTreeView, Qt's QTreeView and KDE System Monitor's tree all do.
//
// A GROUP is a band of rows under an inert CAPTION row, Windows Task
// Manager's "Apps" / "Background processes" and Explorer's group-by. The
// app numbers them; they show in ascending number, never re-sorted.
// Caption rows take no selection, hover, focus or keyboard stop -- they
// have no app row, and every public function still speaks app rows.
typedef int (*uui_table_group_fn)(void *ctx, int row);   // 0..UUI_TABLE_MAX_GROUPS-1
typedef void (*uui_table_group_title_fn)(void *ctx, int group, char *out, int cap);

// A TREE nests a row under its PARENT (an app row, or -1 for a root). A
// parent in a DIFFERENT group does not count: the row is a root of its
// own group. Cycles are cut, never followed.
//
// **THE EXPANDED STATE IS THE APP'S**, as the rows are: the table asks
// `collapsed` and never stores it, because an app row is an index that
// moves on every refresh while the app knows what the row IS (a pid).
// A click on the expander, or Left/Right on the keyboard, leaves the row
// in `toggled`; the app flips its own state and calls set_rows().
typedef int (*uui_table_parent_fn)(void *ctx, int row);
typedef int (*uui_table_collapsed_fn)(void *ctx, int row); // 1 = children hidden

#define UUI_TABLE_MAX_GROUPS 8

// The permutation is a fixed array because Toykit has no allocator.
// Past this many rows the table shows the first UUI_TABLE_MAX_ROWS in
// sorted order and the rest unsorted after them, rather than silently
// dropping any -- see uui_table.c; groups and the tree are off in that
// case. Task Manager's ceiling is the process limit (sys_proc_max()).
#define UUI_TABLE_MAX_ROWS 256
#define UUI_TABLE_MAX_VIEW (UUI_TABLE_MAX_ROWS + UUI_TABLE_MAX_GROUPS)

#define UUI_TABLE_UNSORTED (-1)

// An `order` entry for group g's caption row. Never >= 0, so every
// "is this an app row" test is `>= 0`.
#define UUI_TABLE_CAPTION(g) (-2 - (g))

// A cell's inset from its column's edges -- public because a widget
// laid over a cell (uui_fileview's rename field) has to line up with
// the text the table drew there.
#define UUI_TABLE_PAD_X 4

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
    uui_table_fade_fn fade;  // NULL = no row ever faded
    uui_table_icon_fn icon;  // NULL = no icon column
    uui_table_heat_fn heat;  // NULL = no cell shaded
    uui_table_meter_fn meter; // NULL = no cell metered
    uui_table_group_fn group;             // NULL = no groups
    uui_table_group_title_fn group_title;
    uui_table_parent_fn parent;           // NULL = a flat list
    uui_table_collapsed_fn collapsed;     // NULL = every parent open
    int sort_col;   // UUI_TABLE_UNSORTED, or a column index
    int sort_dir;   // 1 ascending, -1 descending

    // view row -> app row, or UUI_TABLE_CAPTION(g) for a group's caption.
    // Rebuilt by uui_table_set_rows() and by uui_table_set_sort(), which
    // is why an app that already calls set_rows() after refreshing its
    // data needs no other hook. `order_rows` is the row_count it was
    // built for, so a count that changed without a rebuild is detectable
    // rather than silently indexing stale positions. `view_count` is the
    // rows ON SCREEN: captions in, collapsed children out.
    int order[UUI_TABLE_MAX_VIEW];
    int order_rows;
    int view_count;

    // Per APP row, from the last rebuild: its depth in the tree, whether
    // it has children, and its effective parent (-1 = a root). OWNED.
    unsigned char depth[UUI_TABLE_MAX_ROWS];
    unsigned char kids[UUI_TABLE_MAX_ROWS];
    short up[UUI_TABLE_MAX_ROWS];
    int toggled;    // app row whose expander was used, or -1; see take_toggled()
    int structured; // OWNED: groups or a tree are in force this rebuild

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
    int focused;    // OWNED -- driven by the focus ring's set_focused
    int top;        // first visible row; OWNED
    int row_h;      // 0 = derive from the font
    struct uui_sbar sb; // the scrollbar, below the header; OWNED
    // The glide (ui/uui_scrollanim.h): `top` jumps, the rows are drawn
    // displaced for a few frames. OWNED.
    struct uui_scrollanim anim;

    uint32_t bg, fg, sel_bg, sel_fg;
    uint32_t head_bg, head_fg, grid;
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
// See uui_table_fade_fn. NULL turns it off again.
void uui_table_set_fade(struct uui_table *t, uui_table_fade_fn fade);
// See uui_table_icon_fn. NULL turns it off again.
void uui_table_set_icon(struct uui_table *t, uui_table_icon_fn icon);
// See uui_table_heat_fn. NULL turns it off again.
void uui_table_set_heat(struct uui_table *t, uui_table_heat_fn heat);
// See uui_table_meter_fn. NULL turns it off again.
void uui_table_set_meter(struct uui_table *t, uui_table_meter_fn meter);
// Groups and the tree; NULL for either half turns it off. Both rebuild
// the order at once.
void uui_table_set_groups(struct uui_table *t, uui_table_group_fn group,
                           uui_table_group_title_fn title);
void uui_table_set_tree(struct uui_table *t, uui_table_parent_fn parent,
                         uui_table_collapsed_fn collapsed);
// The app row whose expander was clicked or keyed since the last take,
// or -1. Taking clears it.
int  uui_table_take_toggled(struct uui_table *t);
// Rows on screen, captions included and collapsed children not.
int  uui_table_view_count(const struct uui_table *t);
// The group whose CAPTION is at view position `view_row`, or -1 for an
// ordinary row.
int  uui_table_caption_at(const struct uui_table *t, int view_row);

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

// --- colours, RESOLVED --------------------------------------------
//
// **READ A TABLE'S COLOUR THROUGH THESE, NEVER OFF THE STRUCT.** The
// fields hold UUI_COLOR_UNSET until they are drawn (utheme.h), so a
// direct `t->sel_bg` gets the sentinel rather than a colour -- which is
// how uui_fileview drew its marked rows in 0xFF000000 for exactly as
// long as it took a test to notice.
uint32_t uui_table_c_bg(const struct uui_table *t);
uint32_t uui_table_c_fg(const struct uui_table *t);
uint32_t uui_table_c_sel_bg(const struct uui_table *t);
uint32_t uui_table_c_sel_fg(const struct uui_table *t);
uint32_t uui_table_c_head_bg(const struct uui_table *t);
uint32_t uui_table_c_grid(const struct uui_table *t);

int  uui_table_row_h(const struct uui_table *t);
int  uui_table_header_h(const struct uui_table *t);
int  uui_table_visible_rows(const struct uui_table *t);
int  uui_table_scrollbar_visible(const struct uui_table *t);
// The bar, placed and filled in from the table's rows. For a widget that
// draws a table's rows its own way (uui_fileview) and keeps its bar.
struct uui_sbar *uui_table_sbar(struct uui_table *t);
int  uui_table_bar_hover(struct uui_table *t, int cx, int cy, int *on);

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
