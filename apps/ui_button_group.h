#ifndef UI_BUTTON_GROUP_H
#define UI_BUTTON_GROUP_H
#include "ui_button.h"

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
// First real caller: apps/calculator.c's button grid. Notepad's
// Save/Load pair still uses the plain widgets.h path for now -- see
// docs/decisions.md for why that migration was deliberately left for a
// later, separate change rather than folded into this one.
struct ui_button_group {
    struct ui_button *buttons; // not owned -- caller's array; positions are usually font-size-dependent and get refreshed via ui_button_set_geometry() before most calls here (see calculator.c)
    int count;
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

// Clears whichever button was pressed, if any. Safe to call even when
// none was -- gui_apps.h's on_release fires unconditionally whenever a
// press sequence ends.
void ui_button_group_release(struct ui_button_group *g);

// Hit-tests (cx, cy) and returns the matching button's `code`, or -1 if
// none hit -- the caller still decides what a `code` means (calculator.c
// feeds it straight to calc_input()).
int ui_button_group_click(struct ui_button_group *g, int cx, int cy);

#endif
