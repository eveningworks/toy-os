#ifndef UUI_SLIDER_H
#define UUI_SLIDER_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"

// A slider with DISCRETE STOPS -- one per option, not a continuous range.
//
// Written for System Settings' pointer acceleration, where the values
// are ordered levels (off, low, medium, high) and the thing a user wants
// to express is "more" or "less" rather than a number. A radio list
// says the same thing and says nothing about the ORDER; a dropdown hides
// all but one value behind a click. That ordering is the whole argument
// for the control, and it is why this takes the same `options` array the
// radio list and the dropdown take: the same setting can be drawn any of
// the three ways, chosen by /etc/settings.d's `Widget=`.
//
// DISCRETE, NOT CONTINUOUS, and that is not a limitation to fix later.
// The settings registry has one value type that carries a choice list --
// an enum -- so every setting this can serve has a finite, ordered set
// of named values. A continuous slider would need a numeric setting type
// with a range and a unit, which does not exist; building the widget for
// it first would be a control with nothing to control.
//
// THE VALUE IS AN INDEX into `options`, so an app reads it exactly as it
// reads a radio list's `selected` or a dropdown's selection, and a
// setting can change its `Widget=` with no code change anywhere.

struct uui_slider {
    int x, y, w, h;
    const char *const *options; // caller-owned, in order
    int count;
    int selected;   // index, or -1 when there is nothing to select
    int hovered;    // OWNED
    int dragging;   // OWNED -- 1 while the thumb is held
    uint32_t bg, fg, track_bg, fill_bg, thumb_bg;

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

void uui_slider_init(struct uui_slider *s, const char *const *options, int count);
void uui_slider_set_options(struct uui_slider *s, const char *const *options, int count);

// The natural size wants room for the track plus the WIDEST option's
// label underneath -- measured over every option, never just the
// current one, or the control would resize as the user drags it.
void uui_slider_natural_size(const struct uui_slider *s, int *out_w, int *out_h);
void uui_slider_set_geometry(struct uui_slider *s, int x, int y, int w, int h);
void uui_slider_draw(struct ugfx_surface *surf, const struct uui_slider *s);

int  uui_slider_hit(const struct uui_slider *s, int cx, int cy);
// The stop nearest `cx`, which is what a click anywhere on the track
// selects -- as on every real slider, where clicking the track moves the
// thumb rather than doing nothing.
int  uui_slider_stop_at(const struct uui_slider *s, int cx);
int  uui_slider_press(struct uui_slider *s, int cx, int cy);   // 1 if it took the press
int  uui_slider_drag(struct uui_slider *s, int cx, int cy);    // 1 if the value moved
void uui_slider_drag_end(struct uui_slider *s);
int  uui_slider_hover(struct uui_slider *s, int cx, int cy);   // 1 if changed
int  uui_slider_key(struct uui_slider *s, int key);            // Left/Right/Home/End
int  uui_slider_wheel(struct uui_slider *s, int notches);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_slider_focus_ops;
extern const struct uui_widget_ops uui_slider_ops;

#endif
