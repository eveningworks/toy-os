#ifndef UUI_ROUTE_H
#define UUI_ROUTE_H

#include "ui/uui_widget.h"

// uui_route -- pointer input, delivered to widgets by the toolkit
// instead of by every app.
//
// WHY THIS EXISTS
// ---------------
// Widget BEHAVIOUR belongs to the widget (docs/gui-guidelines.md), and
// input handling is behaviour. Before this, an app received a press and
// dispatched it by hand: try the dropdown, then the listbox, then each
// button, remember whether a button is down so motion means "drag",
// remember to call drag_end on release. Calculator wrote none of that
// only because uapp_desc.buttons routed one widget type for it; UI Demo
// wrote twenty-one calls because it uses the rest.
//
// The cost of that arrangement was not verbosity, it was silence: a
// widget whose input an app forgot to forward is not a compile error
// and not a visible defect. It just does nothing, which is exactly how
// `uui_listbox` shipped a scrollbar that drew and could not be dragged.
//
// THE TWO RULES
// -------------
// **1. The pointer GRAB.** Whichever widget consumes a press keeps
// every subsequent motion and the release, wherever the cursor goes --
// including outside its own rect. That single rule is what makes drags
// work with no app state at all: a thumb drag that leaves the scrollbar
// keeps scrolling, and a button press dragged away still gets its
// release (and so can decline to commit, per the commit-on-release
// rule).
//
// **3. A DRAG IS A SESSION THE ROUTER RUNS**, between a source widget
// and whichever widget is under the pointer. A press is a click until
// the pointer moves UUI_DRAG_THRESHOLD with the button held; then the
// grab holder is asked `drag_start` once, and if it fills a payload the
// grab becomes a drag: every motion goes to the widget under the
// pointer as `drag_over` (with a leave to the last one), the release
// becomes `drop` on a target that accepted, and the source hears
// `drag_end`. Within ONE window only -- across windows is the
// compositor's, as `wl_data_device` is Wayland's and not a toolkit's.
// Qt's QDrag/QDropEvent and GTK's drag-motion/drag-drop have this shape.
//
// **2. Input order is the REVERSE of draw order.** Items are hit-tested
// back to front, and a widget with an ACTIVE OVERLAY (an open dropdown
// popup) is offered the press before any hit-testing at all -- its own
// rect does not cover its popup, and the popup is drawn on top of
// everything. Getting this backwards is how a click on a popup lands on
// whatever happens to be underneath it.
//
// WHAT IT IS NOT
// --------------
// Not a scene graph and not a widget hierarchy with parents. It walks a
// flat array of `struct uui_item` -- the same array a `uui_layout`
// already holds -- and recurses only into nested layouts. An app that
// places its widgets by hand (UI Demo, deliberately, so its coordinates
// stay documented for tests) uses the identical mechanism by declaring
// the array and no layout.

// WHY an app is being told a widget changed. Generic -- it names the
// input that arrived, not what the widget decided -- but it is the
// difference between "scrolled with the wheel" and "dragged the thumb",
// which an app that logs or undoes actions genuinely needs and cannot
// recover from widget state alone.
enum uui_reason {
    UUI_REASON_PRESS = 1,
    UUI_REASON_MOTION,
    UUI_REASON_RELEASE,
    UUI_REASON_WHEEL,
    UUI_REASON_KEY,
    UUI_REASON_DROP,   // something was dropped ON this widget; read what
                       // it recorded, and uapp_drag() for the payload
};

struct uui_router {
    struct uui_item *items;   // caller-owned; hit-tested back to front
    int count;

    // The widget holding the pointer grab, or NULL. Set by a press that
    // was consumed, cleared on release.
    void *grab;
    const struct uui_widget_ops *grab_ops;
    int grab_id;

    // --- the drag session (rule 3) -------------------------------------
    int press_x, press_y;      // where the grab's press landed
    int dragging;              // a source's drag_start said yes
    struct uui_drag drag;      // its payload, live
    void *target;              // the widget whose drag_over was last asked
    const struct uui_widget_ops *target_ops;
    int target_id;
    int drop_id;               // set by a release that dropped; read once
                               // through uui_router_take_drop()
    struct uui_drag dropped;   // the payload of that drop -- the live one
                               // is reset with the grab, and the app's
                               // handler runs after

    // ONE ITEM THE TOOLKIT OWNS, above every one of the app's: the shared
    // edit menu (ui/uui_editmenu.h). Consulted ONLY while its overlay is
    // open, and then before anything else -- an overlay owner like a
    // dropdown's, that no app had to declare. NULL for none.
    struct uui_item *extra;
};

// Pointer motion past this many pixels from the press, with the button
// held, is a drag rather than a click -- Windows' SM_CXDRAG is 4, Qt's
// startDragDistance 10; 6 suits an accelerated pointer without eating a
// sloppy click.
#define UUI_DRAG_THRESHOLD 6

// A POINT NO WIDGET CAN CONTAIN, handed to a widget the pointer has
// left so it can clear a highlight (see uui_route.c).
#define UUI_NOWHERE (-(1 << 20))

void uui_router_init(struct uui_router *r, struct uui_item *items, int count);

// Each returns the ID of the widget that consumed the event (0 if none
// consumed it, or if the consumer has no id), and sets *out_changed to
// 1 if anything asked for a repaint. The id is what an app switches on;
// see uapp_desc.on_widget().
// `mods` is the KEY_MOD_* bits held at the press -- the button bits
// are already gone (uapp.c splits the event; see WIN_MOUSE_MODS).
int uui_router_press(struct uui_router *r, int cx, int cy, unsigned mods,
                      int *out_changed);
// `mods` is the KEY_MOD_* bits held NOW, for a drag's copy/move.
int uui_router_motion(struct uui_router *r, int cx, int cy, unsigned buttons,
                       unsigned mods, int *out_changed);
int uui_router_release(struct uui_router *r, int cx, int cy, int *out_changed);

// --- the drag session -------------------------------------------------
int  uui_router_drag_active(const struct uui_router *r);
const struct uui_drag *uui_router_drag(const struct uui_router *r);
// The id of the widget a release just dropped on, or 0; cleared by the
// read. uapp turns it into on_widget(id, UUI_REASON_DROP).
int  uui_router_take_drop(struct uui_router *r);
// The payload of the drop take_drop() just reported; valid until the
// next press.
const struct uui_drag *uui_router_dropped(const struct uui_router *r);
// Esc: the source hears drag_end(0), the target a leave, nothing drops.
void uui_router_drag_cancel(struct uui_router *r);

// A drag from ANOTHER window (WIN_EV_DRAG_OVER/LEAVE/DROP): the same
// target ops, with a payload whose `source` is NULL and whose files are
// in the drag slot. `over` returns 1 while some widget accepts; `drop`
// returns the id of the widget that took it (0 for none), readable
// through uui_router_dropped() as after a local drop.
int  uui_router_extern_over(struct uui_router *r, int cx, int cy, unsigned mods,
                            const char *dir, int count, const char *label);
void uui_router_extern_leave(struct uui_router *r);
int  uui_router_extern_drop(struct uui_router *r, int cx, int cy, unsigned mods);

// The WIN_CURSOR_* the widget tree wants at (cx, cy): deepest declaring
// widget wins, open popup first, DEFAULT when nothing asks.
//
// A QUERY -- consumes nothing -- so uapp calls it on every motion and
// lets uapp_set_cursor() filter the no-ops.
int uui_router_cursor(const struct uui_router *r, int cx, int cy);

// The wheel goes to the widget under the cursor, or to the grab holder
// if there is one. Apps used to send it to a fixed chain of widgets in
// a fixed order, which is why a wheel over one control scrolled another.
// AN OPEN POPUP TAKES THE KEY, wherever the focus ring happens to be.
// A dropdown's popup is drawn over the whole window and its rows are
// what the user is looking at, so Esc, Enter, the arrows and a typed
// letter belong to it and not to whatever was focused before it opened
// -- which is what every real toolkit does, and the keyboard's half of
// the `overlay_active` rule the pointer already follows.
//
// Returns the id of the widget that took the key, or 0 when no overlay
// is open or it declined -- in which case the caller carries on to the
// focus ring as before.
// The id an app knows a widget by, found by POINTER anywhere in the
// item tree; 0 if it is not declared. The focus ring holds widgets and
// no ids -- ids belong to the router, and a second copy of them in the
// ring would be a second thing to keep in step -- so this is how a
// key-driven change gets reported to an app by the same id its clicks
// arrive under.
int uui_router_id_of(const struct uui_router *r, const void *widget);

// The inverse: the item declared with `id`, anywhere in the tree, or
// NULL. Sound only because ids are UNIQUE -- uapp refuses to start an
// app whose items share one (uui_router_duplicate_id()).
struct uui_item *uui_router_item(const struct uui_router *r, int id);

// The first nonzero id declared on two items, or 0. 0 is "unnamed" and
// may repeat.
int uui_router_duplicate_id(const struct uui_router *r);

// Whether any item in the tree uses `ops` -- "does this app declare a
// lone button", which needs an on_action to be heard.
int uui_router_has_ops(const struct uui_router *r, const struct uui_widget_ops *ops);

int uui_router_overlay_key(struct uui_router *r, int key, unsigned mods,
                            int *out_changed);

int uui_router_wheel(struct uui_router *r, int cx, int cy, int notches,
                      int *out_changed);

// Draws every declared widget, then every OVERLAY on top.
//
// Two passes, and the second is the point: a dropdown's popup is drawn
// after all the ordinary widgets, so it lands over them without the app
// ordering anything. "Call draw_popup LAST" used to be a rule an app
// had to remember, and immediate-mode drawing meant forgetting it
// painted the listbox straight over the popup with nothing to notice.
//
// Hidden items are skipped, so hiding a widget removes it from the
// picture and from hit-testing with one flag.
void uui_router_draw(struct uui_router *r, struct ugfx_surface *s);

// The editable text at (cx, cy) -- the deepest visible item whose
// edit_target answers, back to front through containers, as a press
// would find it -- filling `out` and its item's id. 0 for none.
struct uui_edit_target;
int uui_router_edit_at(const struct uui_router *r, int cx, int cy,
                       struct uui_edit_target *out, int *out_id);

// Drops any grab -- for an app that tears down or replaces its widgets
// while the button is held.
void uui_router_reset(struct uui_router *r);

#endif
