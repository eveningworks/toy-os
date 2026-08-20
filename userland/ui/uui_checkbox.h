#ifndef UUI_CHECKBOX_H
#define UUI_CHECKBOX_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"

// The ring-3 checkbox, an OBJECT -- it holds its own geometry, checked
// state and hover, like uui_button/uui_listbox. It used to be three
// free functions told (x, y, size, label, checked, hovered) on every
// call, which left it nothing to keep and nowhere for a layout to
// write. See apps/ui/ui_checkbox.h for the full note; this is a
// faithful mirror of it.

struct uui_checkbox {
    // Content-relative. w/h come from uui_checkbox_natural_size() via
    // init/set_geometry, never from the caller: the clickable area IS
    // the box plus the label, and a caller-supplied size could disagree
    // with what is drawn.
    int x, y, w, h;

    int size;           // edge length of the box
    const char *label;  // not owned; may be NULL

    int checked;        // the value
    int hovered;        // OWNED -- driven by uui_checkbox_hover()
    int focused;        // OWNED -- driven by the focus ring's set_focused
    int disabled;

    // Draw a hover wash behind the box and label? **OFF by default**,
    // which makes the checkbox the one documented exception to
    // docs/gui-guidelines.md's hover contract. The wash covers the whole
    // clickable area -- box plus label -- which on a form of several
    // checkboxes reads as a moving block of colour following the cursor
    // rather than as feedback, and the control's state is already
    // legible from the tick. An app that wants it sets this to 1.
    int hover_effect;

    uint32_t bg, fg;
};

void uui_checkbox_init(struct uui_checkbox *cb, int x, int y, int size,
                        const char *label, uint32_t bg, uint32_t fg);
void uui_checkbox_set_geometry(struct uui_checkbox *cb, int x, int y);
void uui_checkbox_natural_size(const struct uui_checkbox *cb, int *out_w, int *out_h);
void uui_checkbox_draw(struct ugfx_surface *s, const struct uui_checkbox *cb);
int  uui_checkbox_hit(const struct uui_checkbox *cb, int cx, int cy);
// 1 if `hovered` changed, so a caller knows to repaint.
int  uui_checkbox_hover(struct uui_checkbox *cb, int cx, int cy);
// Flips `checked`, returns the new value. Commits on CONTACT rather
// than on release -- correct for a toggle, and the deliberate exception
// to the press-then-commit rule (docs/gui-guidelines.md), since the
// result is instantly visible and instantly reversible.
int  uui_checkbox_toggle(struct uui_checkbox *cb);

// Keyboard: SPACE toggles, and nothing else does anything. That is what
// a checkbox does in Win32, GTK and Qt alike -- Enter is the default
// BUTTON's key there, not the focused control's, so claiming it here
// would take a key the app may want. Returns 1 if the key was consumed.
//
// Committing on a key press is not an exception to
// docs/gui-guidelines.md's press-then-commit rule: that rule exists so a
// press can be cancelled by dragging away before release, and a key has
// no drag. There is nothing to cancel.
int  uui_checkbox_key(struct uui_checkbox *cb, int key);

// Full table with ROUTED POINTER INPUT (ui/uui_route.h): toggling is
// the widget's once this is declared.
struct uui_widget_ops;
extern const struct uui_widget_ops uui_checkbox_ops;

#endif
