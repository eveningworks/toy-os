#ifndef UUI_WIDGET_H
#define UUI_WIDGET_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_describe.h"
#include "keyboard.h"   // KEY_MOD_*, IS_NORDIC_CHAR

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
// --- a drag session's payload (ui/uui_route.h's third rule) ------------
//
// What is being dragged, filled in by the SOURCE widget's drag_start and
// read by every target's drag_over/drop. The toolkit owns the struct;
// the strings it points at are the source's and stay valid until its
// drag_end -- a target that needs them later copies them in drop().
//
// `kind` says what the payload is; a target refuses a kind it does not
// take. Only files exist today. Adding a kind is adding an enum row.
enum uui_drag_kind {
    UUI_DRAG_NONE = 0,
    UUI_DRAG_FILES,   // `dir` + the source widget's marked set or selection
};

struct uui_drag {
    int kind;
    void *source;        // the widget that started it -- a target may
                         // refuse to drop onto the thing being dragged
    int source_id;
    const char *dir;     // UUI_DRAG_FILES: the directory the items are in
    int count;           // ... and how many
    const char *label;   // one item's name, or a "N items" the source wrote
    // Live, updated by the router on every motion:
    int x, y;            // the pointer, content-relative
    unsigned mods;       // KEY_MOD_* held now
    int copy;            // Ctrl is held: the toolkit's copy/move convention
                         // (Explorer's and Dolphin's), derived from `mods`
                         // so every source and target agree on it
    int accepted;        // the widget under the pointer said yes
};

struct uui_widget_ops {
    // The preferred minimum -- see uui_primitives.h. 0 in either axis
    // means "no preference", and a container reads that as "give me
    // whatever is going".
    void (*natural_size)(const void *w, int *out_w, int *out_h);

    // Told where it ended up. A widget whose size is not a free
    // parameter (a radio list, a checkbox) is free to ignore the w/h it
    // is handed and recompute its own -- see uui_radio_list.h.
    void (*set_geometry)(void *w, int x, int y, int width, int height);

    // Where the widget ENDED UP -- the getter set_geometry lacks. Lets a
    // caller iterate a router's widgets and read each rect generically
    // (uapp_log_layout(), so a test can drive a control by asking rather
    // than guessing pixels, instead of every app hand-rolling the same
    // geometry log). Content-relative, like every other rect here.
    // OPTIONAL: a widget a test never drives leaves it NULL.
    void (*bounds)(const void *w, int *x, int *y, int *out_w, int *out_h);

    void (*draw)(struct ugfx_surface *s, const void *w);

    // Anything this widget draws ON TOP of its siblings -- a dropdown's
    // open popup. Painted in a second pass after every widget's `draw`,
    // which is what makes z-order the toolkit's problem rather than a
    // rule every app has to remember ("call draw_popup LAST").
    void (*draw_overlay)(struct ugfx_surface *s, const void *w);

    // Content-relative hit test.
    int (*hit)(const void *w, int cx, int cy);

    // --- focus, for a widget that takes keys ------------------------
    int (*key)(void *w, int key, unsigned mods);
    void (*set_focused)(void *w, int focused);
    int (*accepts_focus)(const void *w);

    // --- pointer input, routed by uui_route.h -----------------------
    //
    // **A widget that fills these in never needs an app to forward
    // anything.** That is the point: before these existed the toolkit
    // routed exactly ONE widget type (a button group, via
    // uapp_desc.buttons), so Calculator wrote no input code at all
    // while UI Demo hand-dispatched twenty-one calls -- and the widget
    // whose scrollbar an app forgot to forward was simply dead.
    //
    // Each returns 1 if it CONSUMED the event (which also means "the
    // app should repaint"). A press that returns 1 additionally takes
    // the POINTER GRAB: every motion and the release go to that widget
    // wherever the cursor then goes, which is what makes a drag work
    // without any app tracking whether a button is down.
    // `mods` is the KEY_MOD_* bits held at the press -- already split
    // out of the event by uapp.c, so a widget never sees the button
    // bits. Ctrl+click and Shift+click are what it is for.
    int (*press)(void *w, int cx, int cy, unsigned mods);
    int (*motion)(void *w, int cx, int cy, unsigned buttons);
    int (*release)(void *w, int cx, int cy);
    int (*wheel)(void *w, int notches);

    // The WIN_CURSOR_* belonging over this widget at (cx, cy). Takes a
    // point because a widget is not uniformly one thing -- a table with
    // an editable column wants the I-beam over that column only.
    // NULL means WIN_CURSOR_DEFAULT, which is why check_widget_ops.py
    // has no rule about this slot: a missing one fails at nothing.
    int (*cursor)(const void *w, int cx, int cy);

    // Does this widget currently own an OVERLAY that is drawn outside
    // its own rect -- an open dropdown popup, say? Such a widget gets
    // every press offered to it FIRST, before hit-testing anything
    // else, because input order has to be the reverse of draw order and
    // its `hit` only covers the closed control. A widget with no
    // overlay leaves this NULL.
    int (*overlay_active)(const void *w);

    // --- containers -------------------------------------------------
    //
    // A widget that HOLDS other items says so here, and the router
    // recurses into them: a container needs no input code of its own,
    // and -- the part that matters -- a child reports its OWN id to the
    // app rather than the container's. An app switching on ID_CHOICES
    // keeps working when the page it is on gains a scroll view.
    //
    // This replaced a type check on uui_layout_ops in uui_route.c. The
    // check worked for exactly one container and silently swallowed
    // every child's id for any other, which is what a scroll view
    // discovered by breaking Control Panel's radio buttons.
    //
    // A container that also has `hit` CLIPS its children to itself: the
    // router will not recurse into a container the pointer is outside.
    // That is what a scroll view needs (its children are laid out
    // beyond its edges and must not be clickable there) and what a
    // plain layout does not want, which is why uui_layout declares no
    // hit at all.
    struct uui_item *(*children)(void *w, int *out_count);

    // Wrapped around the painting of `children`. A container that needs
    // its children CLIPPED does it here, because the router is what
    // draws them -- a scroll view cannot clip what it does not paint.
    // `children_end` is also where a container paints anything that
    // belongs ON TOP of its children and is not one of them, which for
    // a scroll view is its scrollbar.
    //
    // The container's own `draw` still runs first, for a background.
    void (*children_begin)(struct ugfx_surface *s, void *w);
    void (*children_end)(struct ugfx_surface *s, void *w);

    // Report the sub-rects a test drives this widget by -- a menu's
    // titles and popup rows, a strip's slots -- through ui/uui_describe.h.
    // Bounds are reported by the walk from `bounds`; this is for what
    // bounds cannot say. Optional.
    void (*describe)(const void *w, const struct uui_describe *d);

    // --- drag and drop, a ROUTER session (ui/uui_route.h) ------------
    //
    // A SOURCE fills `drag_start`: asked once, when the pointer has
    // moved past the drag threshold with the button still down after a
    // press this widget consumed. Fill `d` and return 1 to start a
    // drag -- the widget's ordinary press gesture is then over (cancel
    // a band or a double-click arm here); return 0 to keep the plain
    // grab, which is what a thumb drag or a rubber band wants.
    int (*drag_start)(void *w, int cx, int cy, struct uui_drag *d);
    // The session ended: dropped somewhere (1) or cancelled (0).
    void (*drag_end)(void *w, int dropped);
    // A TARGET fills `drag_over` and `drop`. drag_over is asked on every
    // motion while the pointer is over this widget, and once with
    // UUI_NOWHERE when it leaves, so a highlight can clear; return 1 to
    // accept a drop HERE. `drop` is called only on a widget whose last
    // drag_over accepted; the router then names it to the app with
    // UUI_REASON_DROP, and the app reads what the widget recorded.
    // tools/check_widget_ops.py: a table with `drop` needs `drag_over`.
    int (*drag_over)(void *w, int cx, int cy, const struct uui_drag *d);
    int (*drop)(void *w, int cx, int cy, const struct uui_drag *d);
    // The source paints what is being carried, at d->x/y, after every
    // overlay -- the toolkit has no drag cursor shape, so the ghost is
    // the whole feedback.
    void (*drag_draw)(struct ugfx_surface *s, const void *w, const struct uui_drag *d);
};

// One thing in a container. `widget` is whatever `ops` expects, not
// owned; the caller's storage must outlive the layout.
struct uui_item {
    const struct uui_widget_ops *ops;
    void *widget;
    unsigned flags;

    // Not drawn, not hit-tested, not focusable -- the widget still
    // exists and keeps its state. This is how an app SHOWS and HIDES a
    // control: set the flag, and both the painter and the router skip
    // it, so a hidden widget cannot be clicked by accident.
    int hidden;

    // The app's name for this widget, reported back through
    // uapp_desc.on_widget() when the widget changes. Ids are the app's
    // to choose and mean nothing to the toolkit; 0 is "don't tell me".
    //
    // An id rather than a per-widget callback pointer because a widget
    // tree here is a const-ish array with no allocator behind it, and
    // one switch in the app reads better than a function pointer and a
    // user pointer stapled to every widget struct. Same shape as
    // uui_menubar's item ids.
    int id;

    // PINS the item's extent along the container's stacking axis,
    // overriding what its natural_size asked for. 0 -- the default --
    // means "ask the widget", which is what every item did before a
    // divider needed to move one. CSS's flex-basis and QSplitter's
    // setSizes(): the size is the CONTAINER'S to decide once something
    // outside the widget owns it, and a widget-by-widget "set my width"
    // would be that answer written once per widget type.
    //
    // Ignored by UUI_GRID, whose cells are uniform by definition, and
    // LAST in this struct because apps initialise it positionally.
    int main_size;

    // The app's name for this widget in the layout log, and NULL for a
    // widget the log leaves out (ui/uui_describe.h). After main_size,
    // so a positional initialiser still lands where it did.
    const char *name;
};

// Stretch to the container's cross-axis size instead of taking the
// natural one. A row of buttons under a wide panel usually wants
// UUI_FILL_W; a fixed-size control does not.
#define UUI_FILL_W 0x01
#define UUI_FILL_H 0x02

// Tell the app about HOVER changes too (on_widget, UUI_REASON_MOTION with
// no button held). Off, a hover is the widget's own business -- Qt's
// setMouseTracking(), for the one view that genuinely needs it.
#define UUI_TRACK_HOVER 0x04

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

// A CHARACTER WITH Ctrl OR Alt HELD IS A SHORTCUT, NOT TEXT: the keyboard
// delivers Ctrl+1 as '1' with KEY_MOD_CTRL (api/keyboard.h), and a field
// that typed it would type on every shortcut the app has. uapp routes one
// past the widgets to on_key; a widget handed keys directly declines it.
// Qt and GTK draw the same line. AltGr is not Alt -- it picks characters.
static inline int uui_key_is_shortcut(int key, unsigned mods) {
    return (mods & (KEY_MOD_CTRL | KEY_MOD_ALT)) &&
           ((key >= 32 && key < 127) || IS_NORDIC_CHAR(key));
}

#endif
