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

    // Driven by the widget from pointer input; an app never sets these.
    int hovered;                // tab index under the cursor, or -1
    int hovered_close;          // 1 if the cursor is on that tab's close box
    int pressed;                // tab index being pressed, or -1
    int pressed_close;

    // What the app is told. Either may be NULL.
    //
    // **`on_close` FIRES ON RELEASE, like every other commit in this
    // toolkit** (docs/gui-guidelines.md): a close box armed on press and
    // committed on release can be cancelled by dragging off it, which
    // matters more here than on an ordinary button because the action
    // destroys something.
    void (*on_select)(void *ctx, int index);
    void (*on_close)(void *ctx, int index);
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

extern const struct uui_widget_ops uui_tabs_ops;

#endif
