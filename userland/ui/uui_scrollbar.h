#ifndef UUI_SCROLLBAR_H
#define UUI_SCROLLBAR_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"


// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// --- scrollbar --------------------------------------------------------

#define UUI_SCROLLBAR_MIN_THUMB_H 16

// Optional stepper arrows at each end, as Vivaldi and the older
// toolkits draw them. OFF by default, so nothing changes for a caller
// that does not ask -- and off is the modern default (neither macOS nor
// GNOME draws them any more). An app that wants them says so; the
// widget stays stateless either way, since an arrow is hit-tested from
// geometry and the scroll position still belongs to whatever is being
// scrolled.
#define UUI_SCROLLBAR_ARROWS 0x01

// A HORIZONTAL bar. Every function below then reads `y`/`h` as the
// cross axis and `x`/`w` as the one being scrolled, and `total_lines`/
// `visible_rows` as COLUMNS -- so one implementation serves both and a
// thumb cannot be drawn in one place and hit-tested in another.
//
// **THE OFFSET RUNS THE OTHER WAY, and that is not an accident of the
// implementation.** A vertical bar here is a SCROLLBACK: 0 is pinned to
// the newest text at the bottom, because that is what a terminal and an
// editor's view want. Horizontally there is no "newest" -- 0 is the
// LEFT MARGIN, as it is in every toolkit -- so a horizontal bar
// measures its offset from the start. Passing a vertical offset to a
// horizontal bar therefore draws the thumb at the wrong end rather than
// merely sideways.
#define UUI_SCROLLBAR_HORIZ  0x02

// The zones keep their vertical names on a horizontal bar: UUI_SB_UP is
// the LEFT arrow and UUI_SB_ABOVE the track left of the thumb. Renaming
// them would have meant two enums for one set of answers.

enum uui_scrollbar_zone {
    UUI_SB_NONE = 0,
    UUI_SB_ABOVE,  // the track above the thumb -- page up
    UUI_SB_THUMB,  // the thumb itself -- start a drag
    UUI_SB_BELOW,  // the track below the thumb -- page down
    UUI_SB_UP,     // the top arrow -- step one line back
    UUI_SB_DOWN,   // the bottom arrow -- step one line forward
};

// How tall each arrow button is, when UUI_SCROLLBAR_ARROWS is set:
// square, so it follows the bar's thickness and stays proportional to
// the font like everything else.
#define uui_scrollbar_arrow_h(w) (w)

// --- the SHAPE --------------------------------------------------------
//
// A radius per part, because the shape of a scrollbar is the APP's
// decision and not this widget's: it is `border-radius` on
// `::-webkit-scrollbar-thumb` in CSS, and the radius a Qt style hands
// `drawRoundedRect()`. UUI_SB_CAPSULE is uui_primitives.h's UUI_CAPSULE,
// half the short axis -- Breeze passes exactly `0.5 * width` for its
// groove and its handle -- and any radius is clamped to that, since a
// corner larger than the rect is not a shape.
#define UUI_SB_CAPSULE UUI_CAPSULE   // ui/uui_primitives.h

struct uui_scrollbar_style {
    int track_radius;   // the groove
    int thumb_radius;   // the handle
};

// What a bar looks like when the app does not say: Breeze's capsule at
// both ends of both parts. A caller wanting something squarer passes
// its own style rather than opting in to this one.
extern const struct uui_scrollbar_style uui_scrollbar_style_default;

// Preferred minimum: the strip's width; no height preference -- a
// scrollbar is as tall as whatever it scrolls. See uui_primitives.h.
//
// The WIDTH IS A PARAMETER of every function below, so an app that wants
// a wider or narrower bar simply passes one -- but it should get its
// default from HERE rather than picking a pixel count, or the bar stops
// tracking the font size (CLAUDE.md's font-derived layout rule) and the
// toolkit's bars quietly stop matching each other. Notepad hardcoding 8
// is what made this worth spelling out.
void uui_scrollbar_natural_size(int *out_w, int *out_h);

// How far the thumb is inset from each side of a `w`-wide strip. Exposed
// because a caller that wants to know where the thumb's PIXELS are (a
// test, mainly) would otherwise re-derive it and drift.
int uui_scrollbar_thumb_inset(int w);

// Draws in uui_scrollbar_style_default. This is the whole API for a
// caller with no opinion about the shape, which is most of them.
void uui_scrollbar_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                         int total_lines, int visible_rows, int scroll_offset,
                         uint32_t track_bg, uint32_t thumb_bg, unsigned flags);

// The same, with the shape named. `style` NULL is the default one, so
// the call above is this one with nothing to say.
void uui_scrollbar_draw_styled(struct ugfx_surface *s, int x, int y, int w, int h,
                                int total_lines, int visible_rows, int scroll_offset,
                                uint32_t track_bg, uint32_t thumb_bg, unsigned flags,
                                const struct uui_scrollbar_style *style);

enum uui_scrollbar_zone uui_scrollbar_hit(int x, int y, int w, int h,
                                           int total_lines, int visible_rows,
                                           int scroll_offset, int px, int py,
                                           unsigned flags);

// The thumb's extent ALONG THE SCROLLED AXIS: (y, h) vertically, and
// (x, w) horizontally when UUI_SCROLLBAR_HORIZ is set.
void uui_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                               int scroll_offset, int *out_thumb_y, int *out_thumb_h,
                               int bar_w, unsigned flags);

// The scroll offset a thumb drag to `py` implies. `grab_offset_in_thumb`
// is how far down the thumb the drag started, so the thumb doesn't jump
// under the cursor on the first pixel of movement.
int uui_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                   int py, int grab_offset_in_thumb,
                                   int bar_w, unsigned flags);

#endif
