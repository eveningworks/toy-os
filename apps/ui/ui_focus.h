#ifndef UI_FOCUS_H
#define UI_FOCUS_H

#include <stdint.h>
#include "ui_primitives.h"

// Keyboard focus for a window's widgets: which one keys go to, Tab and
// Shift-Tab to move between them, and the ring that shows which it is.
//
// **Why this exists.** Widgets that take the keyboard used to be routed
// by trying each in turn -- `if (a_key(...)) ... else if (b_key(...))`.
// That works with one such widget and breaks with two: whichever comes
// first swallows every key it recognises. UI Demo hit this the day
// ui_dropdown landed, because a dropdown handles arrows even while
// CLOSED, so the listbox sitting under it could never be arrowed at
// all. The bug is invisible in a screenshot and obvious the moment you
// press Down.
//
// Focus is also the thing that makes a keyboard-only path possible at
// all: without it there is no answer to "where does this key go" that
// doesn't depend on what the mouse last touched.
//
// ---------------------------------------------------------------------
// How a widget joins
// ---------------------------------------------------------------------
//
// By exporting one `const struct ui_focus_ops`. Nothing central has to
// be edited -- the same property gui_app_registry[] and the Control
// Panel's applet table already have, and the reason this uses a small
// vtable rather than a `switch` over a widget-kind enum: a switch would
// put every widget's name inside this file, and a widget that forgot to
// add its case would fail silently at runtime instead of at the call
// site.
//
//   // in ui_listbox.c
//   const struct ui_focus_ops ui_listbox_focus_ops = { ... };
//
//   // in the app
//   static struct ui_focusable items[] = {
//       { &tbx,  &ui_textbox_focus_ops  },
//       { &dd,   &ui_dropdown_focus_ops },
//       { &list, &ui_listbox_focus_ops  },
//   };
//   ui_focus_init(&focus, items, 3);
//
//   on_key:   ui_focus_key(&focus, key, mods);
//   on_press: ui_focus_click(&focus, cx, cy);   // clicking focuses
//   on_draw:  ui_focus_draw_ring(&focus, ox, oy); // after the widgets
//
// ---------------------------------------------------------------------
// What it deliberately is NOT
// ---------------------------------------------------------------------
//
// Not a WM-level focus system: focus is per-window and lives in the app,
// because the WM already decides which WINDOW has the keyboard and a
// second global would have to agree with it. Not a layout or tab-order
// solver either -- tab order is array order, which is what a caller
// writing the array already controls, and every alternative (positional
// sorting, explicit indices) is more machinery for a list nobody has
// found too long to reorder by hand.

struct ui_focusable;

// What the focus manager needs from a widget. Every entry may be NULL
// except `key`; a widget that supplies nothing else simply doesn't draw
// a ring and can't be focused by clicking.
struct ui_focus_ops {
    // Deliver a key. Return non-zero if the widget consumed it. The
    // manager has already decided this widget has focus, so a widget
    // must NOT check focus itself.
    int (*key)(void *w, int key, uint8_t mods);

    // Is (cx, cy) inside this widget? Used to move focus on a click,
    // and to let a widget with an open popup claim points outside its
    // own resting rect.
    int (*hit)(const void *w, int cx, int cy);

    // Draw the focus ring for this widget. Called after every widget has
    // drawn, so the ring is never painted over. A widget that would
    // rather show focus its own way (a caret, say) can leave this NULL.
    void (*draw_ring)(const void *w, int origin_x, int origin_y, uint32_t color);

    // Can this widget take focus right now? NULL means "always". A
    // disabled widget answers 0 and Tab skips straight over it, which is
    // what every real toolkit does and what stops focus landing
    // somewhere that ignores every key.
    int (*accepts_focus)(const void *w);

    // Told when focus arrives or leaves, for widgets with visible focus
    // state of their own -- ui_textbox activates its caret here, so an
    // app no longer has to remember to call ui_textbox_set_active()
    // alongside focusing it. NULL if there is nothing to do.
    void (*set_focused)(void *w, int focused);
};

struct ui_focusable {
    void *widget;
    const struct ui_focus_ops *ops;
};

struct ui_focus {
    struct ui_focusable *items; // caller-owned array, tab order = array order
    int count;
    int current;    // index of the focused widget, or -1 for none
    uint32_t ring;  // focus-ring colour, from the caller (apps/ui/ stays theme-agnostic)
};

// `current` starts at -1: nothing focused until something is clicked or
// Tab is pressed. Deliberate -- a window that opens with a widget
// already focused steals the first keystroke from whatever the user
// actually meant to click.
void ui_focus_init(struct ui_focus *f, struct ui_focusable *items, int count,
                    uint32_t ring);

// The focused widget, or NULL. `ui_focus_index()` is the same answer as
// an index, for an app that logs or switches on it.
void *ui_focus_current(const struct ui_focus *f);
int ui_focus_index(const struct ui_focus *f);

// Move focus explicitly. Out of range clears focus (index -1) rather
// than clamping -- the same "reject, don't guess" rule the rest of this
// codebase's parsers and ui_listbox_set_selected() follow.
void ui_focus_set(struct ui_focus *f, int index);

// Next/previous widget that accepts focus, wrapping. Skips anything
// whose accepts_focus() says no. Returns 1 if focus moved.
//
// Wrapping is right here and clamping is right in ui_listbox, which is
// not an inconsistency: a tab ring is a cycle with no ends, while a list
// has a first and last item whose boundaries carry meaning.
int ui_focus_next(struct ui_focus *f);
int ui_focus_prev(struct ui_focus *f);

// --- the three calls an app forwards ---------------------------------

// Handles Tab and Shift-Tab itself, then hands anything else to the
// focused widget. Returns 1 if the key was consumed.
//
// **Shift-Tab needs `mods`.** Tab has no shifted character, so the key
// code is 0x09 either way and there is no other way to tell them apart
// -- this is the case that motivated carrying modifier bits at all (see
// api/keyboard.h's "Modifier bits").
int ui_focus_key(struct ui_focus *f, int key, uint8_t mods);

// Moves focus to whichever widget contains (cx, cy), or clears it if
// none does. Call from on_press, BEFORE forwarding the press to the
// widgets themselves, so a widget that acts on the press already has
// focus when it does. Returns 1 if focus changed.
int ui_focus_click(struct ui_focus *f, int cx, int cy);

// Draws the focused widget's ring. Call LAST in on_draw, after every
// widget -- same reasoning as ui_dropdown_draw_popup(): drawing is
// immediate-mode, so anything drawn later would paint over it.
void ui_focus_draw_ring(const struct ui_focus *f, int origin_x, int origin_y);

// The standard ring: a 1px rectangle inset by 2px from the widget's
// rect. Exposed so a widget's own draw_ring can use it for the common
// case instead of each reinventing an outline -- pass the widget's
// content-relative rect and the origin.
void ui_focus_ring_rect(int x, int y, int w, int h,
                         int origin_x, int origin_y, uint32_t color);

#endif
