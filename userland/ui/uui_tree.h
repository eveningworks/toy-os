#ifndef UUI_TREE_H
#define UUI_TREE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_sbar.h"
#include "ui/uui_scrollanim.h"

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

#define UUI_TREE_FILTER_MAX 256 // nodes a filter decides for; the rest show
#define UUI_TREE_MAX_NODES 64 // bounds the collapsed-state bitmap below;
                               // UUI_TREE_CLOSED/OPEN nodes are not in it
                               // and a lazy tree may exceed this count

// A node's parenthood/expansion contract -- see `kind` below.
enum uui_tree_kind {
    UUI_TREE_AUTO = 0, // parent iff the next node is deeper; expansion
                        // state lives in the widget's bitmap (the
                        // original contract, and the zero default)
    UUI_TREE_CLOSED,   // the APP owns expansion (a lazy tree): this node
    UUI_TREE_OPEN,     //   IS a parent and this is its current state
    UUI_TREE_HEADER,   // a SECTION HEADING ("This computer"): drawn as a
                        // caption, never selected, hovered, a parent or a
                        // drop target, and skipped by the arrow keys
};

struct uui_tree_node {
    const char *label;
    // 0 for a top-level row, 1 for its children, and so on. A node is a
    // PARENT if the next node is deeper -- derived rather than declared,
    // so a node cannot claim children it does not have.
    int depth;
    // The app's own identifier for this row, handed back by
    // uui_tree_selected_id(). Opaque here.
    int id;
    // UUI_TREE_AUTO for a static outline. CLOSED/OPEN declare a LAZY
    // parent: its children are simply absent from the array until the
    // app puts them there, so parenthood cannot be derived and the
    // widget must be told. An expander click on one reports through
    // on_toggle instead of flipping the bitmap -- the app relists,
    // rebuilds the array and calls uui_tree_set_nodes_keep(), and the
    // new array's `kind` is the new truth. GtkTreeView's
    // row-expanded/test-expand-row split, minus the model.
    int kind;
    // An icon NAME (lib/icon_cache.h: /usr/share/icons/<name>.qoi), drawn
    // at the text's height before the label; NULL for none. When ANY node
    // has one, every row gets the gutter, so labels at one depth still
    // line up -- the rule uui_sidebar learned for its headings.
    const char *icon;
    // A small STATUS icon over the corner of `icon` -- Windows Device
    // Manager's yellow "!" and down-arrow -- or NULL. Shape as well as
    // colour, so it reads without the colour. After `icon`, so a
    // positional initialiser that stops there still means "none".
    const char *badge;
    // A short note at the row's right edge ("8.7G free", "read-only"),
    // NULL for none; and a usage METER before it when `meter_on` --
    // `meter_pm` per mille full, in `meter_color`. Explorer's This PC
    // rows, shrunk to fit a tree row. After `badge`, for the same
    // positional-initialiser reason.
    const char *note;
    int meter_on, meter_pm;
    uint32_t meter_color;
};

struct uui_tree {
    int x, y, w, h;
    const struct uui_tree_node *nodes; // caller-owned, display order
    int count;

    int selected;    // NODE index, or -1
    int hovered;     // NODE index, or -1; OWNED
    int focused;     // OWNED -- driven by the focus ring's set_focused
    int top;         // first visible VISIBLE-row; OWNED
    int row_h;       // 0 = derive from the font
    struct uui_sbar sb; // the scrollbar; OWNED
    struct uui_scrollanim anim; // the glide (ui/uui_scrollanim.h); OWNED
    // A drop target's state (ui/uui_widget.h's drag ops): the NODE a
    // drag is hovering, -1 for none, drawn as an accent outline; and
    // the node the last drop landed on, for uui_tree_drop_id(). OWNED.
    int drop_node;
    int dropped_node;

    // One bit per node: 1 = collapsed, so its descendants are hidden.
    // A BITMAP rather than a flag on the node, because the nodes are
    // the APP's const array and collapsing is the widget's state --
    // writing into the caller's data would make a `const` array a lie.
    uint64_t collapsed;

    // Set by uui_tree_set_on_toggle(); NULL for a static tree. Called
    // with the node's id and 1 to expand / 0 to collapse.
    void (*on_toggle)(void *ctx, int id, int expand);
    void *toggle_ctx;

    uint32_t bg, fg, sel_bg, sel_fg, guide;

    // THE SELECTION'S STYLE, the app's choice: UUI_SEL_SOFT (0, the
    // default) is the pale wash in sel_bg/sel_fg; UUI_SEL_STRONG fills the
    // row in the theme's accent with its text colour on it -- Windows'
    // and Plasma's selected row, for a tree that is the window's subject.
    // It draws no focus ring on that row: the fill is the indicator.
    int sel_style;

    // A FILTER (uui_tree_set_filter): while one is set, a node shows only
    // if it or something under it matches, and a filtered tree is fully
    // OPEN whatever was collapsed -- a match hidden in a closed branch
    // would be no match at all. Every desktop's tree search does this.
    // `filter_shown` is computed when the filter or the nodes change;
    // nodes past UUI_TREE_FILTER_MAX always show. OWNED.
    char filter[48];
    int (*filter_match)(void *ctx, int node, const char *text);
    void *filter_ctx;
    uint64_t filter_shown[UUI_TREE_FILTER_MAX / 64];
};

// UUI_SEL_ROUNDED is the design language's selection (docs/gui-guidelines.md):
// a soft fill with a 1px edge, rounded and inset from the sides; the
// edge is the full accent while the tree has focus, and is its focus
// ring. Hover is rounded to match.
enum { UUI_SEL_SOFT = 0, UUI_SEL_STRONG = 1, UUI_SEL_ROUNDED = 2 };

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

// set_nodes, but KEEPING the scroll position (clamped) -- what a lazy
// tree's rebuild-on-toggle wants, since dropping `top` would fling the
// view back to the root on every expand. The selection is still only
// clamped: node ids are the app's and may not survive its rebuild, so
// re-selecting is the app's job (uui_tree_select_id()).
void uui_tree_set_nodes_keep(struct uui_tree *t, const struct uui_tree_node *nodes, int count);

// Show only the nodes matching `text`, what leads to them and what is
// under them; "" or
// NULL clears it. `match` decides whether node `node` matches (an app
// that searches more than the label -- ids, drivers -- supplies one);
// NULL matches the label, case-insensitively. A filtered label draws
// its matching run highlighted. Returns how many nodes MATCH.
int  uui_tree_set_filter(struct uui_tree *t, const char *text,
                         int (*match)(void *ctx, int node, const char *text), void *ctx);
// Whether `haystack` contains `needle`, ignoring case -- the label
// match, for an app's own `match` to build on.
int  uui_tree_text_matches(const char *haystack, const char *needle);

// The lazy half of UUI_TREE_CLOSED/OPEN -- see struct uui_tree_node.
// Without a callback, a kind-declared expander is inert.
void uui_tree_set_on_toggle(struct uui_tree *t,
                             void (*fn)(void *ctx, int id, int expand), void *ctx);

// --- what the app asks --------------------------------------------------

// The selected node's `id`, or -1 when nothing is selected. An APP id,
// not a row: rows move as things collapse, ids do not. Anything an app
// stores must be an id.
int  uui_tree_selected_id(const struct uui_tree *t);
// Select by id, expanding whatever was hiding it and SCROLLING it into
// view. Returns 1 if the selection moved. This is how an app restores a
// selection -- selecting a hidden node and leaving it hidden, or
// selected below the fold, would look like nothing happened.
int  uui_tree_select_id(struct uui_tree *t, int id);
// The id of the node the last drop landed on, or -1 -- read from
// on_widget(id, UUI_REASON_DROP). Every node is a drop target; what a
// drop MEANS is the app's (a folder tree moves files into the node).
int  uui_tree_drop_id(const struct uui_tree *t);

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
