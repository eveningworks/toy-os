#ifndef UUI_SPINBOX_H
#define UUI_SPINBOX_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"

// A NUMBER YOU CAN TYPE OR STEP: a text field with up/down steppers,
// bounded by min/max and moved by `step`.
//
// **WHY A NUMBER NEEDS A WIDGET AT ALL.** A slider shows a magnitude and
// cannot show a VALUE -- you can see that the pointer is fastish, not
// that it is at 150%. And it cannot accept one: there is no way to ask
// for exactly 150 by dragging. Every desktop that exposes a numeric
// setting pairs the two for that reason (KDE's System Settings puts a
// spinbox beside every slider); GNOME drops the spinbox and is regularly
// criticised for it. toy-os had neither, which is why `mouse_speed` was
// four named levels -- see kernel/lib/mouse_config.c, which said so.
//
// **IT EMBEDS A uui_textbox RATHER THAN PARSING KEYS ITSELF**, because
// this project has exactly one implementation of what editing means
// (uui_edit -- caret, selection, Ctrl+A, shift-arrows) and a second one
// living in a spinbox would drift from it the first time either gained a
// key. What this adds on top is the steppers, the bounds, and the rule
// that the field always holds a number.
//
// THE COMMIT RULE, which is the part that surprises people: typing does
// NOT change the value. The field holds free text while it is being
// edited -- a half-typed "15" on the way to "150" is out of range and
// must not be clamped to 25 under the user's fingers -- and the value is
// parsed, bounded and adopted on ENTER or when focus leaves. A stepper,
// having no intermediate state, applies immediately. That is what every
// spinbox does and it is the reason `value` and the field's text can
// legitimately disagree for a moment.
//
// An invalid or out-of-range entry REVERTS to the last good value rather
// than being clamped: clamping silently turns "500" into "300" and tells
// the user they got what they asked for.

struct uui_spinbox {
    int x, y, w, h;

    // The ONE editor (ui/uui_edit.h), so a spinbox and a text field
    // cannot disagree about what typing means.
    struct uui_textbox field;

    int value;          // always within min..max
    int min, max, step; // step <= 0 is read as 1
    const char *unit;   // drawn after the number when not being edited

    int hovered;        // 0 none, 1 up, 2 down; OWNED
    int armed;          // the stepper a press landed on; OWNED

    uint32_t bg, fg, border, step_bg;

    // VISIBLE, BUT DOES NOTHING -- the same field uui_button and
    // uui_checkbox already carry. Set it and the control draws dimmed
    // and refuses press/motion/release/key, and drops out of the focus
    // ring so Tab does not stop on something that cannot be used.
    //
    // A DISABLED CONTROL IS ONLY AN IMPROVEMENT WITH A REASON BESIDE
    // IT. Nothing here draws that reason -- the widget has nowhere to
    // put it -- so whoever sets this owes the user a sentence (System
    // Settings prints the registry's `unavailable` text above the
    // control; see abi/setting_abi.h).
    int disabled;
};

// `unit` may be NULL. The field is initialised to `value` rendered as
// text, so a spinbox that is never touched already reads correctly.
void uui_spinbox_init(struct uui_spinbox *s, int value,
                      int min, int max, int step, const char *unit);

// Sets the value programmatically, clamping to the range, and re-renders
// the field. Use this when the app's own state changed -- NOT to respond
// to the user, who is already handled.
void uui_spinbox_set_value(struct uui_spinbox *s, int value);
int  uui_spinbox_value(const struct uui_spinbox *s);

// Adopts whatever is typed in the field: parses it, and either takes it
// (in range) or reverts the field to the current value. Returns 1 if the
// value CHANGED. Called on Enter and on focus loss; an app that closes a
// dialog on a button press should call it first, or the last thing typed
// is lost without a word.
int  uui_spinbox_commit(struct uui_spinbox *s);

int  uui_spinbox_hit(const struct uui_spinbox *s, int cx, int cy);
int  uui_spinbox_press(struct uui_spinbox *s, int cx, int cy);
int  uui_spinbox_motion(struct uui_spinbox *s, int cx, int cy, unsigned buttons);
int  uui_spinbox_release(struct uui_spinbox *s, int cx, int cy);
// Up/Down step; everything else goes to the field. Enter commits.
int  uui_spinbox_key(struct uui_spinbox *s, int key, unsigned mods);
int  uui_spinbox_wheel(struct uui_spinbox *s, int notches);

void uui_spinbox_natural_size(const struct uui_spinbox *s, int *out_w, int *out_h);
void uui_spinbox_set_geometry(struct uui_spinbox *s, int x, int y, int w, int h);
void uui_spinbox_draw(struct ugfx_surface *surf, const struct uui_spinbox *s);

extern const struct uui_widget_ops uui_spinbox_ops;

#endif
