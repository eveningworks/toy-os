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
    //
    // PREFER `rect` BELOW and make this a one-line delegation to
    // wm_overlay_damage(): an overlay that computes its own damage has
    // to remember the shadow AND that its rect may have changed since
    // it was last drawn, and both were got wrong here.
    void (*damage)(void);

    // WHERE IT IS, when it is open. 1 with the rect filled, 0 when it
    // has none (closed, or nothing to show).
    //
    // WHY THE CORE WANTS THIS RATHER THAN A damage() PER OVERLAY. Two
    // things have to be added to an overlay's rect before it is the
    // right damage, and an overlay that spells its own damage can
    // forget either:
    //
    //  - THE SHADOW, which paints OUTSIDE the rect (wm_shadow.h). The
    //    volume flyout used wm_damage_rect() where every other shadowed
    //    popup used wm_damage_window_rect(), and left a ghost of its
    //    shadow behind on close.
    //  - THE RECT IT LAST OCCUPIED. A popup whose size depends on its
    //    contents is somewhere else after they change -- the volume
    //    flyout grows a row per audio stream and is anchored ABOVE the
    //    taskbar, so gaining one moves its TOP UP. Damaging only where
    //    it is now leaves the band it vacated holding the old frame.
    //
    // The core records this rect after it draws the overlay, so the
    // "where was it" half is bookkeeping no overlay has to do. That is
    // what wm_render.c already does for WINDOWS, comparing each one's
    // last-rendered rect against its current one; this is the same idea
    // for the panel's own overlays, and Weston spells it
    // weston_view_damage_below().
    //
    // NULL for an overlay whose damage is genuinely not one rect: the
    // context menu damages a rect per open submenu, and the confirm
    // dialog asks for a full repaint on purpose. Those keep damage().
    int (*rect)(int *x, int *y, int *w, int *h);

    // Live press tracking, every tick, for a control that can be
    // DRAGGED -- a slider, a dialog button that arms on press. NULL
    // when nothing in the overlay is draggable.
    void (*update_press)(int mx, int my, uint8_t buttons);

    // WHEEL NOTCHES, while this overlay is up and the pointer is over
    // it. Returns 1 when it consumed them -- an overlay that scrolls
    // must take the wheel before the focused window does, or a menu
    // over a text view scrolls the text underneath it. Scoped to the
    // overlay's own rect by the overlay, exactly as the tray scopes
    // its volume-by-wheel to the tray.
    int (*wheel)(int mx, int my, int notches);

    // A KEY, while this overlay is up. Returns 1 when it consumed it,
    // which stops it reaching a global shortcut or the focused window --
    // an open menu owns the keyboard, as an xdg_popup's grab does.
    // NULL for an overlay with nothing to type into or navigate.
    //
    // The op is offered in table order, so the most modal overlay sees
    // the key first, and a key nobody wants falls through untouched:
    // an overlay must not swallow what it does not act on, or a
    // shortcut stops working whenever a popup happens to be open.
    int (*key)(int key, uint8_t mods);

    // Dismiss it with no action; safe to call when it is closed. This
    // is what makes the popups MUTUALLY EXCLUSIVE: an open path calls
    // wm_overlay_close_others() instead of naming its peers, so a
    // popup that joins the table is dismissed by every other one --
    // where each open path naming the others by hand left the Super
    // key closing two of four. NULL for a MODAL overlay (the confirm
    // dialog, the file picker), which another popup opening must not
    // dismiss; those two stay up through close_others().
    void (*close)(void);
};

// Draws every open overlay, least modal first. Called once per frame
// from wm_render.c, in place of six named calls. ALSO records where
// each one was drawn, which is what makes the damage above automatic.
void wm_overlay_draw(int mx, int my);

// Damage the named overlay: where it is now, where it was last drawn,
// and the shadow around both. This is what an overlay's damage() op
// should be -- `void volume_damage(void) { wm_overlay_damage("volume"); }`
// -- so the rules live here instead of in ten files. A name that is not
// in the table is a no-op, which is the same failure a missing op is.
void wm_overlay_damage(const char *name);

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

// Offers a key press to each open overlay, most modal first. Returns 1
// when one consumed it.
int wm_overlay_key(int key, uint8_t mods);

// The same for wheel notches, and with the same rule: the pointer's
// position is passed because an overlay only takes the wheel over
// itself.
int wm_overlay_wheel(int mx, int my, int notches);

// The topmost open overlay's name, or NULL when none is up -- for the
// debug console, so a test can ask what is on screen without knowing
// the table.
// Walk the table: every overlay's name and whether it is up. `gui
// state` reports from this, so a new overlay appears there the day it
// is added to the table rather than when somebody remembers.
int wm_overlay_count(void);
const char *wm_overlay_name(int i);
int wm_overlay_is_open(int i);

const char *wm_overlay_topmost(void);

// Closes every overlay with a `close` op except the one named `keep`
// (NULL keeps none: every dismissable popup goes). Called from each
// popup's open path, from the Super key, and when the usable area
// changes.
void wm_overlay_close_others(const char *keep);

// A POPUP OPENED FROM ANOTHER OVERLAY names it as its PARENT, and
// close_others() then spares both. That is Wayland's popup chain -- an
// xdg_popup does not dismiss the surface it hangs off -- and it is what
// keeps the Start menu up while the right-click menu for one of its rows
// is open, as Windows does. NULL clears it; the child MUST clear it when
// it closes, or the next close_others() spares an overlay nobody meant
// to keep.
void wm_overlay_set_parent(const char *name);
// Is ANY overlay up? What the lease policy asks: an overlay is drawn by
// this compositor, and a compositor that is not presenting cannot show it.
int wm_overlay_any_open(void);
const char *wm_overlay_parent(void);

// Places a popup of (w, h) whose preferred top-left is (want_x,
// want_y) -- a tray flyout's right edge on its item, a context menu at
// the pointer -- inside the usable screen: WM_POPUP_MARGIN from the
// left and right edges and from the taskbar, never above the top.
// One rule for the four popups that each clamped by hand. Adopting it
// moved the calendar and a taskbar-anchored context menu up 4 px (their
// gap was 0), and the tray flyouts' right-edge clamp in by 4 (it was 8).
#define WM_POPUP_MARGIN 4
void wm_popup_place(int want_x, int want_y, int w, int h, int *out_x, int *out_y);

#endif
