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
};

struct uui_router {
    struct uui_item *items;   // caller-owned; hit-tested back to front
    int count;

    // The widget holding the pointer grab, or NULL. Set by a press that
    // was consumed, cleared on release.
    void *grab;
    const struct uui_widget_ops *grab_ops;
    int grab_id;
};

void uui_router_init(struct uui_router *r, struct uui_item *items, int count);

// Each returns the ID of the widget that consumed the event (0 if none
// consumed it, or if the consumer has no id), and sets *out_changed to
// 1 if anything asked for a repaint. The id is what an app switches on;
// see uapp_desc.on_widget().
int uui_router_press(struct uui_router *r, int cx, int cy, int *out_changed);
int uui_router_motion(struct uui_router *r, int cx, int cy, unsigned buttons,
                       int *out_changed);
int uui_router_release(struct uui_router *r, int cx, int cy, int *out_changed);

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

// Drops any grab -- for an app that tears down or replaces its widgets
// while the button is held.
void uui_router_reset(struct uui_router *r);

#endif
