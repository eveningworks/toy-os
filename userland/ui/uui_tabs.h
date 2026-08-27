#ifndef UUI_TABS_H
#define UUI_TABS_H

#include <stdint.h>
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

// A ROW OF TABS -- one selected, the rest waiting, each optionally with
// a close box. Konsole's, GNOME Terminal's, Windows Terminal's: the tab
// strip is the application's, not the window manager's.
//
// **THE CALLER OWNS THE TABS**, exactly as uui_fileview owns its entry
// array. This widget holds a pointer to a caller-supplied array and a
// count, and never allocates -- which is what lets a Terminal keep its
// sessions in whatever shape it likes and hand this one only the
// labels. A caller that reorders or closes one edits its own array and
// tells the widget the new count.
//
// **A LABEL IS NOT OWNED AND MUST OUTLIVE THE WIDGET**, the same rule
// uui_button's label follows. A Terminal points these at its sessions'
// own title buffers, so a title arriving from the shell (an OSC
// sequence, api/ansi.h) shows up with nothing copied.
//
// **THE SELECTED TAB IS FILLED WITH THE PAGE, NOT WITH A CONTROL
// COLOUR.** `page_bg` is what sits directly below the strip, and the
// selected tab takes it, rounds its top corners and drops its bottom
// edge -- so it reads as the page reaching up rather than as a button
// above it. That is Windows Terminal's and Konsole's selected tab, and
// it is the one thing that makes a strip over a BLACK terminal look
// like a terminal instead of like a form. The widget derives its ink
// from that colour's luminance, so a dark page gets light labels
// without the caller choosing them.

// What the widget needs to know about one tab. Deliberately not a
// "session" or a "page": this widget draws a strip and reports clicks,
// and knows nothing about what a tab CONTAINS.
struct uui_tab {
    const char *label;  // not owned
    int closable;       // draw a close box and report clicks on it
};

struct uui_tabs {
    int x, y, w, h;             // content-relative

    struct uui_tab *tabs;       // NOT owned -- the caller's array
    int count;
    int selected;

    // What the strip sits on. See the header comment: the selected tab
    // is filled with it and merges into it. Defaults to the theme's
    // window background at init.
    uint32_t page_bg;

    // A "+" PINNED AT THE RIGHT END -- GtkNotebook's action widget and
    // QTabBar's corner widget, and the placement GNOME Terminal and
    // macOS Terminal use. It takes its width out of the strip BEFORE
    // the tabs share what is left, so it never moves as tabs open: a
    // button that walks across the strip is a target you have to look
    // for. Off by default, so an existing caller is unchanged.
    int show_new;

    // Driven by the widget from pointer input; an app never sets these.
    int hovered;                // tab index under the cursor, or -1
    int hovered_close;          // 1 if the cursor is on that tab's close box
    int hovered_new;            // 1 if the cursor is on the "+"
    int pressed;                // tab index being pressed, or -1
    int pressed_close;
    int pressed_new;

    // What the app is told. Either may be NULL.
    //
    // **`on_close` FIRES ON RELEASE, like every other commit in this
    // toolkit** (docs/gui-guidelines.md): a close box armed on press and
    // committed on release can be cancelled by dragging off it, which
    // matters more here than on an ordinary button because the action
    // destroys something.
    void (*on_select)(void *ctx, int index);
    void (*on_close)(void *ctx, int index);
    void (*on_new)(void *ctx);   // the "+"; commits on release like the rest
    void *ctx;
};

void uui_tabs_init(struct uui_tabs *t, struct uui_tab *tabs, int count,
                    void *ctx);

// Repositions without disturbing hover or press state -- the same
// reason uui_button has a separate one.
void uui_tabs_set_geometry(struct uui_tabs *t, int x, int y, int w, int h);

// Preferred minimum: one row of text plus font-derived padding, and
// wide enough for every label at its natural width. A strip narrower
// than that SHRINKS its tabs rather than overflowing -- a tab that is
// off the right edge cannot be clicked, which is the failure
// uui_layout's overflow rule exists to prevent and which a strip can
// avoid because its items are interchangeable.
void uui_tabs_natural_size(const struct uui_tabs *t, int *out_w, int *out_h);

// The strip's own height, which is what a caller needs to lay the PAGE
// out below it. Font-derived; never a pixel constant.
int uui_tabs_height(void);

// Select by index, clamped. Reports through on_select only when the
// selection actually MOVES -- an app that re-selects the current tab on
// every frame must not be told about it every frame.
void uui_tabs_select(struct uui_tabs *t, int index);

// The rect of one tab, content-relative, for a test that wants to click
// a specific one without re-deriving the strip's arithmetic. Returns 0
// for an out-of-range index.
int uui_tabs_rect(const struct uui_tabs *t, int index,
                   int *x, int *y, int *w, int *h);

// The "+" button's rect, same purpose. Returns 0 when show_new is off.
int uui_tabs_new_rect(const struct uui_tabs *t, int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_tabs_ops;

#endif
