#ifndef UI_CHECKBOX_H
#define UI_CHECKBOX_H

#include <stdint.h>

// A small square box, checked/unchecked, with an optional label to its
// right -- e.g. `widget_checkbox_draw(x, y, 14, checked, "Word wrap",
// bg, fg)`. Originally lived in apps/widgets.c/.h; moved here (same
// content, no behavior/rename change) alongside the rest of that file's
// widgets -- see docs/decisions.md. Unlike every other widget that made
// this move, there was no second hand-rolled implementation to
// consolidate first when this was added (see CHANGELOG.md's build
// 490) -- built ahead of an actual caller, by explicit choice, for
// whenever a future settings-style app needs one. Kept exactly as
// minimal as everything else here so it's cheap to have sitting
// unused: draw + hit-test only, no group/mutual-exclusivity logic
// (that's a radio-button concept, not this).

// Total clickable width for a checkbox at `size` with `label` (or just
// `size` if `label` is NULL) -- shared by draw()/hit() so they always
// agree on the same geometry, same pattern as ui_scrollbar.h's widgets.
int widget_checkbox_width(int size, const char *label);

// Draws the box (outlined in `fg`; filled with `fg` too, inset, when
// `checked`) at (x, y), sized `size` x `size`, then `label` (if any)
// in `fg` on `bg` to its right, vertically centered against the box.
// `hovered` washes the box+label area one step via ui_state_bg(), the
// same vocabulary every other control uses. Pass 0 when the caller
// doesn't track hover -- which is what every caller did before this
// parameter existed, so behaviour is unchanged unless you opt in.
//
// Added because docs/gui-guidelines.md requires a hover state on
// anything clickable and this had none: it is act-on-contact (correct
// for a toggle) but that is about WHEN it commits, not about whether it
// admits to being clickable.
void widget_checkbox_draw(int x, int y, int size, int checked, int hovered,
                           const char *label, uint32_t bg, uint32_t fg);

// 1 if (px, py) falls inside the box+label's combined clickable area
// (widget_checkbox_width()'s width, by max(size, a text row's height)
// tall), 0 otherwise.
int widget_checkbox_hit(int x, int y, int size, const char *label, int px, int py);

#endif
