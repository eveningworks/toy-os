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
// **A TAB IS AS WIDE AS ITS TITLE, CAPPED, AND THE TABS PACK LEFT** --
// Konsole's and Windows Terminal's strip, and Chrome's: the leftover
// strip stays empty rather than one tab stretching across it. Only
// when the natural widths no longer fit does the strip fall back to
// EQUAL SHARES with a floor, which is what keeps every tab reachable.
//
// **WIDTHS ARE FROZEN WHILE THE POINTER IS IN THE STRIP** (Chrome's
// rule), because a title arrives from the shell asynchronously and a
// tab that resized under a pointer travelling to its close box would
// move the target. They are recomputed when the pointer leaves, or when
// the COUNT changes -- a tab opening or closing relays out at once.
//
// **THE SELECTED TAB IS A LIGHT FILL WITH AN ACCENT BAR ON TOP**, the
// way VS Code marks one (`tab.activeBorderTop`): the tab takes the
// theme's field colour, rounds its top corners, and carries a 2px accent
// along its TOP edge. Resting tabs are filled with the theme's TAB_REST
// colour -- darker than the strip's own ground, so they read as
// recessed and the selected tab as raised out of them.
//
// Both of those are corrections, and the reason is worth keeping: the
// accent used to sit on the BOTTOM edge, where it is a thin blue line
// directly above a terminal's black page and has almost no contrast,
// and resting tabs used to be the strip's own colour, which left the
// selected one lifted by ten units. Three tabs were hard to tell apart.
//
// This deliberately does NOT try to merge the tab into the page by
// filling it with the page's own colour (Windows Terminal's and
// Konsole's selected tab). That was built and rejected: against this
// theme's near-white chrome a terminal's black page makes the selected
// tab a black block in a light strip, and the merge is not worth it.
// Every colour here is the THEME's, so a dark mode moves the strip with
// everything else -- see docs/decisions/gui.md.

// What the widget needs to know about one tab. Deliberately not a
// "session" or a "page": this widget draws a strip and reports clicks,
// and knows nothing about what a tab CONTAINS.
#define UUI_TABS_FREEZE_MAX 32

struct uui_tab {
    const char *label;  // not owned
    int closable;       // draw a close box and report clicks on it
};

struct uui_tabs {
    int x, y, w, h;             // content-relative

    struct uui_tab *tabs;       // NOT owned -- the caller's array
    int count;
    int selected;

    // A "+" PINNED AT THE RIGHT END -- GtkNotebook's action widget and
    // QTabBar's corner widget, and the placement GNOME Terminal and
    // macOS Terminal use. It takes its width out of the strip BEFORE
    // the tabs share what is left, so it never moves as tabs open: a
    // button that walks across the strip is a target you have to look
    // for. Off by default, so an existing caller is unchanged.
    int show_new;

    // **NUMBER THE TABS: `1: label`, `2: label`.** Konsole's default tab
    // format (`%n: %d`) and iTerm2's, and it answers two questions at
    // once -- the highest number is how many there are, the lit one is
    // where you are. It earns its place when labels COLLIDE, which for a
    // terminal is the common case rather than the odd one: every tab
    // sitting in the same directory reports the same title.
    //
    // Drawn by the widget rather than baked into the caller's labels,
    // because a label is not owned (see above) and a shell rewrites its
    // own asynchronously. Suppressed at one tab, where a number says
    // nothing. Off by default.
    int numbered;

    // **FLOATING: the tabs sit IN a taller bar** rather than on its foot
    // -- four rounded corners and no baseline, Firefox's and libadwaita's
    // tab bar. The caller draws the line under the bar. Off by default.
    int floating;

    // Driven by the widget from pointer input; an app never sets these.
    int hovered;                // tab index under the cursor, or -1
    int hovered_close;          // 1 if the cursor is on that tab's close box
    int hovered_new;            // 1 if the cursor is on the "+"
    int pressed;                // tab index being pressed, or -1
    int pressed_close;
    int pressed_new;

    // The frozen layout (see the header comment): valid while
    // frozen_n == count and frozen_n > 0. Capped rather than allocated,
    // because this widget owns no memory; past the cap the strip simply
    // does not freeze.
    int frozen_n;
    int frozen_w[UUI_TABS_FREEZE_MAX];

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
// the sum of every tab's natural (capped) width. A strip narrower than
// that SHRINKS its tabs to equal shares rather than overflowing -- a
// tab that is off the right edge cannot be clicked, which is the
// failure uui_layout's overflow rule exists to prevent and which a
// strip can avoid because its items are interchangeable.
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
