#ifndef UUI_SWITCH_H
#define UUI_SWITCH_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"

// AN ON/OFF SWITCH -- a sliding knob in a capsule, with the state spelled
// beside it ("On"/"Off", Windows 11's shape; KDE and GNOME show the knob
// alone). For a SETTING that takes effect as a state; a checkbox stays the
// control for an option in a form or a dialog.
//
// Toggles on CONTACT and on Space, exactly as uui_checkbox does and for the
// same reason: the result is visible and reversible at once, so there is
// nothing for commit-on-release to protect (docs/gui-guidelines.md).

struct uui_switch {
    // Content-relative. w/h are the natural size, never the caller's: the
    // clickable area is the track plus the state text.
    int x, y, w, h;

    int on;             // the value
    int hovered;        // OWNED
    int focused;        // OWNED -- the focus ring's set_focused
    int disabled;

    // Spelled after the track; NULL for none. Default "On"/"Off".
    const char *on_text, *off_text;

    uint32_t bg, fg;    // UUI_COLOR_UNSET = the theme's, resolved per draw
};

void uui_switch_init(struct uui_switch *sw, int on);
void uui_switch_natural_size(const struct uui_switch *sw, int *out_w, int *out_h);
void uui_switch_set_geometry(struct uui_switch *sw, int x, int y);
void uui_switch_draw(struct ugfx_surface *s, const struct uui_switch *sw);
int  uui_switch_hit(const struct uui_switch *sw, int cx, int cy);
// Flips `on` and returns it; a disabled switch stays put.
int  uui_switch_toggle(struct uui_switch *sw);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_switch_ops;

#endif
