#ifndef UI_CHECKBOX_H
#define UI_CHECKBOX_H

#include <stdint.h>

// A small square box, checked/unchecked, with an optional label to its
// right.
//
// **This is an OBJECT now** -- it holds its own geometry, its checked
// state and its hover state, like ui_button/ui_listbox/ui_textbox. It
// used to be three free functions taking (x, y, size, label, checked,
// hovered) on every call, which meant it had nowhere to keep any of
// that and nothing else could ask where it was. That is fine for a
// control an app positions by hand and forgets; it is not enough to be
// a layout child, which is what docs/uapp-design.md needs it to be.
//
// The conversion is also what retires the `widget_checkbox_*` naming:
// every OBJECT here is `ui_<widget>_*`, and what keeps the `widget_`
// prefix is now a stated rule rather than an accident --
// **`widget_*` means a stateless helper that is not a layout child**
// (widget_hit(), the scrollbar). See ui_scrollbar.h for why the
// scrollbar is deliberately still one.
//
// Deliberately minimal, as it always was: no group or
// mutual-exclusivity logic. That is a radio-button concept and
// ui_radio_list.h has it.

struct ui_checkbox {
    // Content-relative geometry, the same convention as every other
    // widget here. `w`/`h` are derived from ui_checkbox_natural_size()
    // by init/set_geometry rather than stored from the caller: a
    // checkbox's clickable area IS its box plus its label, and a
    // caller-supplied size could disagree with what is drawn -- which
    // is the bug the shared-geometry rule exists to prevent.
    int x, y, w, h;

    int size;           // edge length of the box itself, in pixels
    const char *label;  // not owned -- must outlive the checkbox; may be NULL

    int checked;        // the value. Read it; ui_checkbox_toggle() flips it
    int hovered;        // OWNED -- driven by ui_checkbox_hover()
    int disabled;       // 1 = drawn dimmed, ignores input

    // Caller-supplied, so this file stays theme-agnostic -- the
    // standing rule for everything under apps/ui/.
    uint32_t bg, fg;
};

// Sets every field, clearing checked/hovered/disabled. Call once when
// the control is created, not every frame -- same reasoning as
// ui_button_init(): geometry is font-dependent and gets recomputed, and
// re-running init would wipe live state. Use ui_checkbox_set_geometry()
// to reposition.
void ui_checkbox_init(struct ui_checkbox *cb, int x, int y, int size,
                       const char *label, uint32_t bg, uint32_t fg);

// Repositions without touching checked/hovered. `w`/`h` are recomputed
// from the natural size, since they are not free parameters.
void ui_checkbox_set_geometry(struct ui_checkbox *cb, int x, int y);

// Preferred minimum: the box, the gap and the label, by the taller of
// the box and one text row. See ui_primitives.h.
void ui_checkbox_natural_size(const struct ui_checkbox *cb, int *out_w, int *out_h);

// Draws at the control's own position, offset by the window's content
// origin -- the two-part addressing every widget here uses. Hover
// washes the whole clickable area (box AND label) via ui_state_bg(),
// because a highlight smaller than the target it describes is a lie
// about where to click.
void ui_checkbox_draw(const struct ui_checkbox *cb, int origin_x, int origin_y);

// 1 if the content-relative point falls in the box+label area.
int ui_checkbox_hit(const struct ui_checkbox *cb, int cx, int cy);

// Updates `hovered`. Returns 1 if it changed, so a caller knows whether
// to repaint. Pass (-1, -1) when the cursor leaves.
int ui_checkbox_hover(struct ui_checkbox *cb, int cx, int cy);

// Flips `checked` and returns the new value. A checkbox commits on
// CONTACT rather than on release -- correct for a toggle, and the
// deliberate exception to the press-then-commit rule in
// docs/gui-guidelines.md, since the result is instantly visible and
// instantly reversible.
int ui_checkbox_toggle(struct ui_checkbox *cb);

#endif
