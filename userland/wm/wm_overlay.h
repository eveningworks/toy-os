#ifndef WM_OVERLAY_H
#define WM_OVERLAY_H

#include <stdint.h>

// The panel's overlays -- the Start menu, the context menu, the two
// tray popups, the file picker, the confirm dialog -- as a REGISTRY
// rather than as six names repeated in four places.
//
// WHAT WENT WRONG WITHOUT IT. Each overlay was wired into the frame
// loop by hand, and the hover half was the easiest to forget because
// forgetting it fails SILENTLY: a mouse move alone takes the
// compositor's cursor-only path, so a highlight derived from the live
// pointer inside a draw is painted only when something ELSE asks for a
// frame -- the clock, once a second. The volume flyout shipped that
// way, and the context menu had been that way since it was written.
// Two of the six tracked a hovered element and damaged their own rect,
// two more did the same from a different call site under a different
// condition, one forced a FULL-SCREEN repaint per mouse move, and one
// did nothing at all. Six overlays, four arrangements, one of them a
// bug.
//
// WHY A TABLE MAKES FORGETTING LOUD. The same rows drive DRAWING and
// CLICKS as well as hover, so an overlay left out of the table does not
// appear on screen and cannot be clicked -- which nobody ships. That is
// the whole argument for including the other two verbs: a registry you
// can forget to join is the problem restated.
//
// WHAT REAL SYSTEMS DO. In Wayland the compositor sends
// wl_pointer.enter/leave/motion to whatever surface is under the
// pointer -- BEING a surface is the registration, and there is nothing
// to forget; KWin and Mutter get the same from hit-testing a scene
// graph. X11 is the counter-example this file is named after: an X
// window receives EnterNotify only if it asked for it in its event
// mask, and "forgot to select the event" is a classic X bug. toy-os's
// overlays are not surfaces (the panel draws them straight into the
// compositor's own buffer), so a scene graph is not available to fall
// out of; this table is the smallest thing that gives the same
// guarantee.

struct wm_overlay {
    const char *name;          // `gui state` reports it; keep it short

    // Is it up? Overlays keep their own flag (`start_menu_open` and
    // friends), which stays public because tests and other overlays
    // read it.
    int (*is_open)(void);

    // Draw it. Called for every open overlay in REVERSE table order, so
    // the most modal one is painted last and therefore on top.
    void (*draw)(int mx, int my);

    // A left click. Returns 1 when routing must stop here -- which is
    // NOT the same as "was it inside me": a dismissing click on the
    // taskbar deliberately falls through, so the Start button acts on
    // the same click that closed a popup.
    int (*handle_click)(int mx, int my);

    // WHICH control the pointer is over, as an OPAQUE TOKEN: any two
    // controls must give different values, the same control the same
    // value, and 0 means none. The core compares it against the last
    // one and calls damage() when it changes -- which is the entire
    // mechanism, and the reason no overlay needs its own copy of it.
    // NULL for an overlay with no hover feedback.
    int (*hover_at)(int mx, int my);

    // Damage its own rect. Required when hover_at is set; the core
    // never damages the whole screen for a hover.
    void (*damage)(void);

    // Live press tracking, every tick, for a control that can be
    // DRAGGED -- a slider, a dialog button that arms on press. NULL
    // when nothing in the overlay is draggable.
    void (*update_press)(int mx, int my, uint8_t buttons);
};

// Draws every open overlay, least modal first. Called once per frame
// from wm_render.c, in place of six named calls.
void wm_overlay_draw(int mx, int my);

// Offers a left click to each open overlay, most modal first. Returns 1
// when one of them consumed it.
int wm_overlay_click(int mx, int my);

// Updates every open overlay's hovered control, damaging the ones that
// changed. Returns 1 when anything changed.
//
// NOTHING RE-HOVERS WHILE THE PRIMARY BUTTON IS DOWN, stated once here
// rather than guarded per overlay: a control being dragged or armed
// must not hand its highlight to whatever the pointer passes over.
int wm_overlay_hover(int mx, int my, uint8_t buttons);

// Per-tick press tracking for every open overlay that wants it.
void wm_overlay_press(int mx, int my, uint8_t buttons);

// The topmost open overlay's name, or NULL when none is up -- for the
// debug console, so a test can ask what is on screen without knowing
// the table.
const char *wm_overlay_topmost(void);

#endif
