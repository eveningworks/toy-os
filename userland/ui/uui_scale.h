#ifndef UUI_SCALE_H
#define UUI_SCALE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

// A CONTINUOUS value on a range, dragged with the pointer -- a media
// player's position bar, a volume control, a percentage.
//
// **THE SPLIT FROM `uui_slider` IS DISCRETE vs CONTINUOUS**, and it is
// GTK's: GtkScale is this, and a widget for an ordered enum is a
// different control. `uui_slider`'s value is an INDEX into a caller's
// option array and it draws a tick per stop, which is exactly wrong for
// a number that has no stops -- 200 ticks on a three-minute song.
//
// It carries NO LABEL. A scale's readout is text the app already knows
// how to format ("1:23 / 3:45", "60%") and where it belongs differs per
// app, so it goes in a `uui_label` or a status bar beside this. That
// also sidesteps uui_meter's trap: a widget whose height depends on
// which strings happen to be set changes size as its value does.
//
// **A DRAG NEEDS THE BUTTON STILL DOWN, and the grab is not enough to
// know that.** The widget holds the pointer grab from its press to its
// release, and a motion can arrive inside that window with nothing
// held -- which would move the value to wherever the pointer is. This
// consults the `buttons` mask, as `uui_button` does.
//
// **DRAGGING REPORTS EVERY MOTION**, so an app can choose. Volume acts
// on UUI_REASON_MOTION and follows the thumb live; a seek acts on
// UUI_REASON_RELEASE, because re-seeking a decoder per pixel is work
// nobody asked for. A widget that only reported the release could not
// do the first.

struct uui_scale {
    int x, y, w, h;

    // The range and where in it. `value` is CLAMPED into [min, max] by
    // every setter here, so an app doing arithmetic on a duration that
    // turns out to be zero cannot push the thumb off the track.
    long min, max, value;

    // Arrow-key and wheel increment, and the PageUp/PageDown one.
    // 0 means "derive": one part in a hundred of the range, and ten of
    // those for a page -- which is right for a percentage and right for
    // a song, and is why an app usually sets neither.
    long step, page;

    int hovered;    // OWNED
    int dragging;   // OWNED
    int focused;    // OWNED -- driven by the focus ring's set_focused
    int disabled;

    uint32_t track_bg, fill_bg, thumb_bg;
};

void uui_scale_init(struct uui_scale *s, long min, long max, long value);

// Changing the range re-clamps the value: a player whose track ends and
// whose next one is shorter must not keep a position past the end.
void uui_scale_set_range(struct uui_scale *s, long min, long max);
void uui_scale_set_value(struct uui_scale *s, long value);
long uui_scale_value(const struct uui_scale *s);

void uui_scale_natural_size(const struct uui_scale *s, int *out_w, int *out_h);
void uui_scale_set_geometry(struct uui_scale *s, int x, int y, int w, int h);
void uui_scale_draw(struct ugfx_surface *surf, const struct uui_scale *s);

int  uui_scale_hit(const struct uui_scale *s, int cx, int cy);

// The value a click at `cx` means -- rounded, so both ends are
// reachable without hitting one exact pixel.
long uui_scale_value_at(const struct uui_scale *s, int cx);

int  uui_scale_press(struct uui_scale *s, int cx, int cy);
int  uui_scale_drag(struct uui_scale *s, int cx, int cy);
void uui_scale_drag_end(struct uui_scale *s);
int  uui_scale_hover(struct uui_scale *s, int cx, int cy);
int  uui_scale_wheel(struct uui_scale *s, int notches);
int  uui_scale_key(struct uui_scale *s, int key);

extern const struct uui_widget_ops uui_scale_ops;
extern const struct uui_widget_ops uui_scale_focus_ops;

#endif
