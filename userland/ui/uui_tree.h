#ifndef UUI_TREE_H
#define UUI_TREE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"

// A TREE: rows at a depth, with collapsible parents.
//
// Written for System Settings' sidebar (KDE System Settings' shape --
// a navigation tree on the left, a page on the right), and deliberately
// general enough for the second caller the roadmap already names: a file
// manager's directory pane. That is this toolkit's standing bar for a
// new widget -- a second REAL caller, not a plausible one -- and it is
// why nothing here mentions settings.
//
// THE NODES ARE THE APP'S, FLAT AND STATIC. The app supplies one array
// in display order, each node carrying its DEPTH; the widget derives
// parent/child from the depth run, exactly as an indented outline reads
// on screen. No allocation, no ownership, no teardown -- the same reason
// uui_menubar's menus are const arrays pointing at each other rather
// than a built tree, and it still holds now that ring 3 HAS malloc.
//
// What the widget OWNS is what a tree does: which rows are collapsed,
// which is selected, where the view is scrolled, and the mapping from a
// screen row to a node once collapsing has hidden some. An app that
// tried to own those would be reimplementing the widget, which is what
// docs/gui-guidelines.md's "behaviour belongs to the component" forbids.

#define UUI_TREE_MAX_NODES 64 // bounds the collapsed-state bitmap below

struct uui_tree_node {
    const char *label;
    // 0 for a top-level row, 1 for its children, and so on. A node is a
    // PARENT if the next node is deeper -- derived rather than declared,
    // so a node cannot claim children it does not have.
    int depth;
    // The app's own identifier for this row, handed back by
    // uui_tree_selected_id(). Opaque here.
    int id;
};

struct uui_tree {
    int x, y, w, h;
    const struct uui_tree_node *nodes; // caller-owned, display order
    int count;

    int selected;    // NODE index, or -1
    int hovered;     // NODE index, or -1; OWNED
    int top;         // first visible VISIBLE-row; OWNED
    int row_h;       // 0 = derive from the font
    int bar_w;
    int thumb_grab;  // -1 when no drag is in progress; OWNED

    // One bit per node: 1 = collapsed, so its descendants are hidden.
    // A BITMAP rather than a flag on the node, because the nodes are
    // the APP's const array and collapsing is the widget's state --
    // writing into the caller's data would make a `const` array a lie.
    uint64_t collapsed;

    uint32_t bg, fg, sel_bg, sel_fg, track_bg, thumb_bg, guide;
};

// THE EASY PATH IS THREE LINES, and it is the one most apps want:
//
//     static const struct uui_tree_node NODES[] = {
//         { "Appearance", 0, CAT_APPEARANCE },
//         {   "Fonts",    1, PAGE_FONTS },
//         {   "Cursor",   1, PAGE_CURSOR },
//         { "System",     0, CAT_SYSTEM },
//         {   "Startup",  1, PAGE_STARTUP },
//     };
//     uui_tree_init(&tree, 0, 0, 0, 0, NODES, 5);
//     ... declare it in uapp_desc.widgets, then read
//     uui_tree_selected_id(&tree) in on_widget().
//
// Everything starts EXPANDED and the first node is selected, so a tree
// that is never told anything else already looks and behaves right --
// collapsing, ids and keyboard navigation are there when an app wants
// them and cost nothing when it does not. The indentation in the
// initialiser above is a comment to the reader; `depth` is what counts.
void uui_tree_init(struct uui_tree *t, int x, int y, int w, int h,
                    const struct uui_tree_node *nodes, int count);
void uui_tree_set_nodes(struct uui_tree *t, const struct uui_tree_node *nodes, int count);

// --- what the app asks --------------------------------------------------

// The selected node's `id`, or -1 when nothing is selected. An APP id,
// not a row: rows move as things collapse, ids do not. Anything an app
// stores must be an id.
int  uui_tree_selected_id(const struct uui_tree *t);
// Select by id, expanding whatever was hiding it. Returns 1 if the
// selection moved. This is how an app restores a selection -- selecting
// a hidden node and leaving it hidden would look like nothing happened.
int  uui_tree_select_id(struct uui_tree *t, int id);

int  uui_tree_is_parent(const struct uui_tree *t, int node);
int  uui_tree_is_collapsed(const struct uui_tree *t, int node);
// Collapse/expand a parent. A no-op on a leaf, so a caller need not check.
int  uui_tree_set_collapsed(struct uui_tree *t, int node, int collapsed);
// Every parent at once. `uui_tree_collapse_all()` then selecting an id
// is the "show me just my section" gesture a settings sidebar wants.
void uui_tree_expand_all(struct uui_tree *t);
void uui_tree_collapse_all(struct uui_tree *t);

// How many rows are currently VISIBLE (collapsed subtrees excluded), and
// the node index at visible row `row`. An app needs neither for ordinary
// use -- they are here because a tree that hid them would force any
// caller wanting to drive it from a test to re-derive the mapping, and
// tools/ does exactly that.
int  uui_tree_visible_count(const struct uui_tree *t);
int  uui_tree_node_at_row(const struct uui_tree *t, int row);

// --- geometry, drawing, input ------------------------------------------

int  uui_tree_row_h(const struct uui_tree *t);
int  uui_tree_visible_rows(const struct uui_tree *t);
int  uui_tree_scrollbar_visible(const struct uui_tree *t);
void uui_tree_natural_size(const struct uui_tree *t, int *out_w, int *out_h);
void uui_tree_draw(struct ugfx_surface *s, const struct uui_tree *t);

// NODE index at (cx, cy), or -1 if outside / on the scrollbar. Returns
// an INDEX, so a caller routing this must convert -- `ops->hit` is a
// BOOLEAN and node 0 is falsey, which is the bug uui_listbox and
// uui_table both shipped. See CLAUDE.md; uui_tree_ops writes `>= 0`.
int  uui_tree_hit(const struct uui_tree *t, int cx, int cy);
// 1 if the point is on the expander triangle rather than the label --
// which is what makes clicking the triangle toggle without also
// navigating, as every real tree does.
int  uui_tree_hit_expander(const struct uui_tree *t, int cx, int cy);

int  uui_tree_hover(struct uui_tree *t, int cx, int cy);   // 1 if changed
int  uui_tree_click(struct uui_tree *t, int cx, int cy);   // selects/toggles; 1 if changed
int  uui_tree_wheel(struct uui_tree *t, int notches);      // 1 if scrolled

int  uui_tree_press(struct uui_tree *t, int cx, int cy);
int  uui_tree_drag(struct uui_tree *t, int cx, int cy);
void uui_tree_drag_end(struct uui_tree *t);
// Up/Down move the selection over VISIBLE rows; Left collapses (or
// steps to the parent when already collapsed) and Right expands (or
// steps to the first child) -- the arrow behaviour every desktop tree
// has, and the reason a tree is not a listbox with indentation.
int  uui_tree_key(struct uui_tree *t, int key);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_tree_focus_ops;
extern const struct uui_widget_ops uui_tree_ops;

#endif
