#ifndef UUI_PATHBAR_H
#define UUI_PATHBAR_H

#include "ui/ugfx.h"
#include "ui/uui_textbox.h"

// A BREADCRUMB PATH: the folder you are in as a row of buttons, one per
// level, each of which takes you there -- Explorer's address bar,
// Dolphin's URL navigator, GTK's path bar. A click past the last
// segment (or uui_pathbar_begin_edit(), Ctrl+L in an app) turns it into
// a text field holding the whole path, which Enter commits and Esc
// abandons: both desktops do exactly this, because typing a path and
// clicking one are two different jobs.
//
// **THE WIDGET NAVIGATES NOTHING.** A click or an Enter parks the path
// for uui_pathbar_take(); the app decides whether it exists and goes
// there, then calls uui_pathbar_set_path() with where it actually is.
// A path the app refuses leaves the field up, holding what was typed.
//
// Too narrow for every segment, the first ones fold into "..." after
// the root, and that button goes to the parent of the first one shown.

#define UUI_PATHBAR_MAX   128
#define UUI_PATHBAR_SEGS  24

struct uui_pathbar {
    int x, y, w, h;
    char path[UUI_PATHBAR_MAX];
    const char *root_label;   // what "/" is called ("System"); NULL = "/"
    const char *root_icon;    // icon_get() name beside it; NULL = none

    int hot;      // hovered segment, UUI_PATHBAR_ELIDED, or -1; OWNED
    int armed;    // pressed segment awaiting its release, or -1; OWNED
    char taken[UUI_PATHBAR_MAX]; // a navigation, until taken
    int has_taken;

    int editing;               // 1 while it is a text field
    struct uui_textbox edit;
    int focused;               // OWNED -- the focus ring's set_focused
};

#define UUI_PATHBAR_ELIDED (-2)   // the "..." button

void uui_pathbar_init(struct uui_pathbar *p, const char *root_label, const char *root_icon);
// Shows `path`. Ends an edit in progress: the app calls this after it
// has navigated, and a field left open over the new place would lie.
void uui_pathbar_set_path(struct uui_pathbar *p, const char *path);
// Opens the text field over the current path, all of it selected so
// typing replaces it.
void uui_pathbar_begin_edit(struct uui_pathbar *p);
void uui_pathbar_end_edit(struct uui_pathbar *p);
int  uui_pathbar_is_editing(const struct uui_pathbar *p);
// The path a click or an Enter asked for, copied to `out`: 1, or 0 when
// nothing was asked since the last call. An Enter's is the typed text,
// unresolved -- relative paths are the app's to resolve.
int  uui_pathbar_take(struct uui_pathbar *p, char *out, int cap);

// Segment `i`'s rect (0 is the root) as currently laid out, and its
// path. For tests and layout logs; 0 when that segment is not shown.
int  uui_pathbar_segment_rect(const struct uui_pathbar *p, int i,
                               int *x, int *y, int *w, int *h);
int  uui_pathbar_segment_count(const struct uui_pathbar *p);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_pathbar_ops;

#endif
