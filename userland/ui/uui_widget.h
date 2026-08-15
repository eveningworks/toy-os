#ifndef UUI_WIDGET_H
#define UUI_WIDGET_H

#include <stdint.h>
#include "ui/ugfx.h"

// uui_widget_ops -- the ONE table a widget exports to be handled
// generically: laid out, drawn, hit-tested, focused.
//
// **One table, not several.** The widget audit in docs/uapp-design.md
// warned that a widget declaring itself in two places is a widget whose
// two declarations can drift -- and `hit` is the obvious casualty,
// since a layout and a focus ring would each want it and they must
// agree. So this supersedes the separate `uui_focus_ops`: focus is a
// few more optional slots on the same table rather than a second one.
//
// **Every slot is optional.** A NULL is not an error and not a special
// case: an item with no `draw` simply reserves space, one with no `hit`
// is not clickable, one with no `key` never takes focus. That is the
// same rule uapp's callbacks follow, and it is what lets a new
// capability arrive as a new slot without touching a widget that does
// not want it.
struct uui_widget_ops {
    // The preferred minimum -- see uui_primitives.h. 0 in either axis
    // means "no preference", and a container reads that as "give me
    // whatever is going".
    void (*natural_size)(const void *w, int *out_w, int *out_h);

    // Told where it ended up. A widget whose size is not a free
    // parameter (a radio list, a checkbox) is free to ignore the w/h it
    // is handed and recompute its own -- see uui_radio_list.h.
    void (*set_geometry)(void *w, int x, int y, int width, int height);

    void (*draw)(struct ugfx_surface *s, const void *w);

    // Content-relative hit test.
    int (*hit)(const void *w, int cx, int cy);

    // --- focus, for a widget that takes keys ------------------------
    int (*key)(void *w, int key, unsigned mods);
    void (*set_focused)(void *w, int focused);
    int (*accepts_focus)(const void *w);
};

// One thing in a container. `widget` is whatever `ops` expects, not
// owned; the caller's storage must outlive the layout.
struct uui_item {
    const struct uui_widget_ops *ops;
    void *widget;
    unsigned flags;
};

// Stretch to the container's cross-axis size instead of taking the
// natural one. A row of buttons under a wide panel usually wants
// UUI_FILL_W; a fixed-size control does not.
#define UUI_FILL_W 0x01
#define UUI_FILL_H 0x02

// --- a custom item ----------------------------------------------------
//
// An app's own drawing, participating in a layout like any widget: it
// reserves the size it asks for and paints itself. Calculator's display
// area is the first one -- an app should not have to choose between
// "use the layout" and "draw something the toolkit has no widget for".
struct uui_custom {
    int x, y, w, h;   // x/y filled by the layout; w/h are the request
    void (*draw)(struct ugfx_surface *s, const struct uui_custom *c);
    void *state;      // the app's, untouched by the toolkit
};

extern const struct uui_widget_ops uui_custom_ops;

#endif
