#ifndef UUI_SIDEBAR_H
#define UUI_SIDEBAR_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

// A NAVIGATION SIDEBAR: bold section headings with selectable items
// under them.
//
// **WHY THIS IS NOT `uui_tree`, WHICH IT LOOKS LIKE.** A tree models
// containment -- a parent HAS children, it can be collapsed, and
// selecting it means something. A navigation sidebar models grouping: a
// heading is a caption over the items beneath it, it is not a
// destination, and collapsing it away would hide the only things the
// user came for. The two read alike on screen and behave nothing alike,
// which is why every desktop that ships one ships it separately:
// KDE's System Settings, GNOME Settings and macOS System Settings all
// use a flat list with inert section headers, not an outline view. That
// is the shape this copies.
//
// Concretely, a heading here:
//   - draws in the BOLD weight and takes no indent,
//   - cannot be selected, hovered or reached with the arrow keys,
//   - is skipped by the focus ring,
//   - and never changes what the app is showing.
//
// An item is the opposite of all five, and a TOP row is a heading in
// the first respect and an item in the other four. That asymmetry is
// the widget:
// an app declares a flat array and gets a sidebar in which only the
// destinations are reachable, which is the property a tree cannot give
// it without pretending a heading is a place.
//
// **What it deliberately does NOT do**, so nobody goes looking: no
// collapsing (see above), and no nesting past one level of grouping (a
// sidebar that needs a second level wants a tree, and should say so).
//
// It DOES have icons now, on headings. That paragraph used to say it
// did not, "because adding a picture per row means an image decoder
// first" -- true when it was written, and untrue since userland/lib/
// uimg.c and the icon cache landed. The comment outlived its fact,
// which is a thing this tree has now caught four times.

#define UUI_SIDEBAR_MAX_ROWS 64

enum uui_sidebar_kind {
    // A caption over the rows below it. Bold, inert, unreachable.
    UUI_SIDEBAR_HEADING = 0,
    // A destination. Indented, selectable, focusable, arrow-navigable.
    UUI_SIDEBAR_ITEM    = 1,
    // **A DESTINATION THAT IS ALSO A TOP-LEVEL ROW.** Bold and
    // unindented like a heading, selectable like an item -- the third
    // of the four combinations, and the one a category with exactly
    // ONE page needs: collapsing it to a single row is right (its
    // heading and its page said the same word), but rendering that row
    // as an ITEM indents it under whatever heading came before and it
    // reads as a page of the wrong category. GNOME's sidebar is a flat
    // list of exactly these.
    UUI_SIDEBAR_TOP     = 2,
    // **A RULE BETWEEN GROUPS, NOT A ROW.** Drawn as a hairline and
    // nothing else: no label, no icon, never hovered, never selected,
    // stepped over by the arrows. A flat sidebar has no captions to
    // show where one group of pages ends, so this carries the grouping
    // instead -- the separators macOS's System Settings uses and GNOME
    // puts between its panel runs. It still occupies ONE ROW of the
    // uniform grid: the row pitch is what every hit test, the scroll
    // offset and the app's own `y` report are derived from, and a
    // short row would have to be special-cased in all four.
    UUI_SIDEBAR_SEP     = 3,
};

struct uui_sidebar_row {
    const char *label;
    int kind;   // enum uui_sidebar_kind
    // AN ICON NAME, NOT A PATH, and NULL for none -- the same rule
    // `Icon=` follows in a .desktop entry: `icon_get()` resolves it
    // under /usr/share/icons and caches the decoded, scaled result.
    //
    // ON HEADINGS ONLY, deliberately. A category is a stable thing worth
    // recognising by shape, and there are a handful of them; the rows
    // under it come and go with what is registered, and an icon per
    // setting is twenty pieces of art whose wrong or generic answers
    // read worse than no picture at all. An icon on an item is ignored
    // rather than refused, so a caller that sets one gets a plain row
    // rather than a surprise.
    //
    // A missing file is not an error either: no decode means no icon
    // means the row draws exactly as it did before, which is what keeps
    // this optional in fact and not just in principle.
    const char *icon;
    // The app's own identifier, handed back by uui_sidebar_selected_id().
    // Meaningless on a heading, which can never be selected.
    int id;
};

struct uui_sidebar {
    int x, y, w, h;
    const struct uui_sidebar_row *rows; // caller-owned, display order
    int count;

    int selected;   // ROW index, or -1; never a heading
    int hovered;    // ROW index, or -1; OWNED, never a heading
    int focused;    // OWNED -- driven by the focus ring's set_focused
    int top;        // first visible row; OWNED
    int row_h;      // 0 = derive from the font
    int bar_w;
    int thumb_grab; // -1 when no drag is in progress; OWNED

    uint32_t bg, fg, heading_fg, sel_bg, sel_fg, track_bg, thumb_bg;
};

// Everything starts scrolled to the top with the FIRST ITEM selected --
// not the first row, which is usually a heading and can never be
// selected. A sidebar that is never told anything else already looks
// and behaves right.
void uui_sidebar_init(struct uui_sidebar *s, int x, int y, int w, int h,
                       const struct uui_sidebar_row *rows, int count);

// Replaces the rows. KEEPS THE SELECTED ID if that id still names an
// item, which is what a rebuild wants: System Settings rebuilds its
// sidebar whenever the setting registry changes, and a rebuild that
// silently jumped back to the top would move the user off the page they
// were editing.
void uui_sidebar_set_rows(struct uui_sidebar *s,
                           const struct uui_sidebar_row *rows, int count);

// --- what the app asks --------------------------------------------------

// The selected item's `id`, or -1. An APP id, not a row index: rows
// move when the sidebar is rebuilt and ids do not.
int uui_sidebar_selected_id(const struct uui_sidebar *s);

// Selects the item with this id. Returns 1 if it was found; a heading's
// id is never matched, because a heading is not a destination.
int uui_sidebar_select_id(struct uui_sidebar *s, int id);

// --- input --------------------------------------------------------------

int uui_sidebar_hit(const struct uui_sidebar *s, int cx, int cy);
int uui_sidebar_press(struct uui_sidebar *s, int cx, int cy);
int uui_sidebar_motion(struct uui_sidebar *s, int cx, int cy, unsigned buttons);
int uui_sidebar_release(struct uui_sidebar *s, int cx, int cy);
int uui_sidebar_wheel(struct uui_sidebar *s, int notches);

// Up/Down move between ITEMS, stepping over headings entirely -- which
// is the whole point of the kind distinction reaching the keyboard as
// well as the mouse. Home/End go to the first/last item.
int uui_sidebar_key(struct uui_sidebar *s, int key, unsigned mods);

// Row pitch and how many rows fit. Public because a debug dump wants to
// report where a row LANDS -- a test that re-derived that in Python
// would be asserting against its own arithmetic rather than the app's
// (the reason DebugConsole.menu_row() exists).
//
// There is no row-index indirection to go with them, and that is the
// point of a flat list: the row at screen position `r` is `top + r`,
// always. A tree needs uui_tree_node_at_row() because collapsing hides
// rows; nothing here hides anything.
int uui_sidebar_row_h(const struct uui_sidebar *s);
int uui_sidebar_visible_rows(const struct uui_sidebar *s);

void uui_sidebar_natural_size(const struct uui_sidebar *s, int *out_w, int *out_h);
void uui_sidebar_set_geometry(struct uui_sidebar *s, int x, int y, int w, int h);
void uui_sidebar_draw(struct ugfx_surface *surf, const struct uui_sidebar *s);

extern const struct uui_widget_ops uui_sidebar_ops;

#endif
