#ifndef UI_BUTTON_GROUP_H
#define UI_BUTTON_GROUP_H
#include "ui_button.h"

struct ui_focus_ops;

// Owns the "which button (if any) is currently pressed" state and the
// hit-testing loop for a fixed set of ui_buttons -- the part every app
// with more than one button used to hand-roll itself (calculator.c's
// g_pressed_index + a private button_at(), before this file). Doesn't
// own the struct ui_button array's memory -- the caller keeps that (a
// static array sized for its own layout, same as calculator.c's old
// BUTTONS[] table always did), so this works for any layout (a grid, a
// row of two buttons) without knowing anything about rows/columns
// itself.
//
// First real caller: apps/calculator.c's button grid; apps/notepad.c's
// Open.../Save As... toolbar followed in the separate, later change
// this comment used to describe as still pending.
struct ui_button_group {
    struct ui_button *buttons; // not owned -- caller's array; positions are usually font-size-dependent and get refreshed via ui_button_set_geometry() before most calls here (see calculator.c)
    int count;

    // Keyboard state, both OWNED -- read-only from app code, and both
    // meaningless unless the group is in a ui_focus ring (ui_focus.h).
    //
    // `focus_index` is which button the arrows are on, or -1. It lives
    // here rather than as a third flag on struct ui_button because it is
    // a property of the GROUP's navigation: exactly one button can hold
    // it, which a per-button flag would let callers violate.
    int focus_index;
    // Code of a button activated by Space/Enter, or -1. Collected via
    // ui_button_group_take_activated() rather than acted on inside the
    // key handler, so a keyboard activation reaches the app through the
    // same path a mouse release does instead of a second callback.
    int activated;
};

void ui_button_group_init(struct ui_button_group *g, struct ui_button *buttons, int count);
void ui_button_group_draw(const struct ui_button_group *g, int origin_x, int origin_y);

// Direct WM event adapters -- shaped to drop straight into gui_apps.h's
// on_press/on_click/on_release (see calculator.c for the thin wrappers
// that just forward into these). All positions are content-relative,
// matching those callbacks' own cx/cy.

// Re-hit-tests (cx, cy) and updates every button's `pressed` flag to
// match (so dragging off one button onto another re-presses correctly,
// same behavior calculator.c's on_press had before this file). Returns
// 1 if which button is "hot" changed (redraw needed), 0 otherwise --
// same contract gui_apps.h's on_press documents.
int ui_button_group_press(struct ui_button_group *g, int cx, int cy);

// Re-hit-tests (cx, cy) with NO button held and updates every button's
// `hovered` flag to match. Returns 1 if which button is hovered changed
// (redraw needed), 0 otherwise -- exactly gui_apps.h's on_hover
// contract, so an app's on_hover can be a one-line forward into this
// (see calculator.c/notepad.c). Pass the (-1, -1) the WM sends when the
// cursor leaves the window straight through: nothing is hit there, so
// the highlight clears on its own with no special case.
int ui_button_group_hover(struct ui_button_group *g, int cx, int cy);

// Clears whichever button was pressed, if any, and returns that
// button's `code` -- or -1 if none was pressed. Safe to call even when
// none was: gui_apps.h's on_release fires unconditionally whenever a
// press sequence ends.
//
// **That return value is how a button is supposed to commit.** A press
// that has been dragged off its button has already cleared the
// `pressed` flag (ui_button_group_press() re-hit-tests every tick), so
// releasing there returns -1 and the action is silently cancelled --
// which is the whole point of press-then-commit-on-release
// (docs/gui-guidelines.md). Committing from on_click instead fires on
// button-DOWN and can never be cancelled; both Calculator and Notepad
// did exactly that until this return value existed.
int ui_button_group_release(struct ui_button_group *g);

// The code of a button activated by Space/Enter since the last call, or
// -1. Call it right next to ui_button_group_release() in on_release and
// act on either -- a keyboard activation then reaches the app through
// exactly the path a mouse click already does, instead of needing a
// parallel callback that apps would inevitably implement differently.
int ui_button_group_take_activated(struct ui_button_group *g);

// Joins a ui_focus ring (see ui_focus.h). The whole group is ONE focus
// stop with arrows moving between its buttons -- a row of related
// controls is one stop in every real toolkit, and it keeps the tab ring
// as short as the app's structure actually is.
extern const struct ui_focus_ops ui_button_group_focus_ops;

// There is deliberately NO ui_button_group_click() (hit-test a point,
// return that button's code). There was, and both its callers were
// wired to gui_apps.h's on_click -- which fires on button-DOWN, so both
// Calculator and Notepad committed on press and could never be
// cancelled by dragging away. Both moved to the release path above and
// it was left with no callers, so it went, per this directory's
// standing rule that a mechanism needs a real caller.
//
// If an act-on-contact control ever genuinely wants one back (the
// narrow case gui_apps.h's on_click describes -- placing a text cursor,
// focusing a field), bring it back then, and make sure that's really
// what it is: "the button should light up when clicked" is not it.

#endif
