#ifndef UUI_PRIMITIVES_H
#define UUI_PRIMITIVES_H

#include <stdint.h>
#include "ui/ugfx.h"
// Every widget reaches its default colours through the theme (UUI_COLOR
// / UTHEME_*), so the one header they all include carries it.
#include "ui/utheme.h"

// Split out of the single uwidgets.c/.h this used to be, one file per
// widget -- the same shape as apps/ui/, so a widget's kernel-side and
// ring-3 versions live at matching paths. See ui/uui.h.

// uui -- the ring-3 port of apps/ui/'s widget primitives, drawn with
// ugfx into a client's own window buffer.
//
// WHAT MOVED, AND WHAT DELIBERATELY DIDN'T
// ----------------------------------------
// This is a port of `ui_primitives` + `ui_button` + `ui_button_group`,
// which is exactly the set apps/calculator.c is built from -- not the
// whole of apps/ui/. The rest (scrollback, listbox, dropdown, textview,
// the focus ring) follows when a client needs it, which is the same
// "second real caller" bar apps/ui/ itself is held to (CLAUDE.md).
// Porting the lot up front would be inventing an API for nobody.
//
// The BEHAVIOUR is carried over verbatim, because it is the part worth
// keeping. In particular:
//
//   * The four interaction states and their derivation from the
//     control's own colour, NOT a fixed palette (see uui_state_bg).
//     The kernel version's comment records why: an early version always
//     LIGHTENED for hover, which on this near-white theme moved the
//     pixels by two out of 255 -- a hover nobody could see. The shift
//     direction has to follow the base colour's luminance.
//
//   * A press commits on RELEASE, and a press dragged off its button
//     does not commit (uui_button_group_release returns -1). That is
//     the rule every control in this GUI follows
//     (docs/gui-guidelines.md), and a ring-3 client is not exempt just
//     because its event source is a message queue rather than a
//     callback.
//
// What is NOT carried over is the coupling to the window manager: these
// take a ugfx_surface and content-relative coordinates, so they have no
// idea a window exists. That is what makes them usable from a process
// that genuinely cannot see the WM.

// --- interaction states ----------------------------------------------

enum uui_state {
    UUI_STATE_REST = 0, // idle -- the control's own colours
    UUI_STATE_HOVER,    // cursor over it, no button held
    UUI_STATE_PRESSED,  // held down AND cursor still over it
    UUI_STATE_DISABLED, // visible, does nothing
};

// `base` shifted for `state` -- a wash toward black for a light control
// and toward white for a dark one, decided from `base`'s own luminance
// rather than assumed. See the header comment for why that matters.
uint32_t uui_state_bg(uint32_t base, enum uui_state state);

// 1 if (px, py) is inside the rect. Coordinate space is the caller's --
// this is a bounds check and nothing more.
int uui_hit(int x, int y, int w, int h, int px, int py);

// --- the rounded rect --------------------------------------------------

// Half the SHORT axis -- a capsule. Any radius larger than that is
// clamped to it, since a corner bigger than the rect is not a shape.
#define UUI_CAPSULE (-1)

// A filled rect with `radius` corners, antialiased 16 sub-samples to
// the pixel. **The arcs BLEND against what is already on the surface,
// so the caller must have painted under the rect in this same pass** --
// blending against a stale back buffer darkens the corner a little
// every frame.
void uui_fill_round_rect(struct ugfx_surface *s, int x, int y, int w, int h,
                         int radius, uint32_t c);

// GLASS: `c` laid OVER what is already there, `fill_a` inside and
// `edge_a` on a 1px rounded edge -- Windows 11's desktop selection, and
// anything else that must tint a picture rather than hide it. Same
// rule as above: the caller painted under it in this pass.
void uui_glass_round_rect(struct ugfx_surface *s, int x, int y, int w, int h,
                          int radius, uint32_t c, uint8_t fill_a, uint8_t edge_a);

// A PANE OF GLASS: inside the rounded rect, `c` laid at `tint_a` over
// `backdrop` (ugfx.h's: a surface-sized buffer in surface coordinates --
// a blur of the scene, a blurred wallpaper) or, when NULL, over what the
// surface holds. The arcs blend against the surface, as above.
void uui_backdrop_round_rect(struct ugfx_surface *s, int x, int y, int w, int h,
                             int radius, const uint32_t *backdrop, uint32_t c,
                             uint8_t tint_a);

// --- natural size ------------------------------------------------------
//
// The ring-3 half of apps/ui/ui_primitives.h's contract, same rule and
// same reasoning -- read that one for the full statement. In short:
//
//     void uui_<widget>_natural_size(const struct ..., int *w, int *h);
//
// is the PREFERRED MINIMUM, the smallest size at which the widget looks
// right; a layout may hand it more. **0 means NO PREFERENCE** and
// callers must handle it -- a text field has no intrinsic width, so it
// writes 0 for width and a real height that must be honoured. Padding
// is font-derived, never a pixel constant.
#define UUI_PAD_X (ugfx_char_w())
#define UUI_PAD_Y (ugfx_char_h() / 2)


// --- keyboard focus ----------------------------------------------------

// A 1px ring in the theme's ACCENT (utheme.h's focus role), around
// (x, y, w, h). Every widget that can hold keyboard focus draws its
// indicator through this one call, so focus looks like one thing --
// and NOT like hover, which is a wash of the control's own colour.
//
// THE CALLER PASSES THE RECT, because only the widget knows its own
// shape: a list rings the focused ROW and falls back to the box when
// nothing is selected, a slider rings its thumb. A ring around the
// whole widget would be a 300px box on a table.
//
// See docs/decisions.md for why the accent rather than a derived tint.
void uui_focus_ring(struct ugfx_surface *s, int x, int y, int w, int h);

// Fills the rect with `bg` shifted for `state`, then centres `label`
// (may be NULL) in `fg`. Pressed draws a darker fill plus a 1px
// down-right nudge of the label -- the darker fill alone reads as a
// colour change rather than a press.
void uui_button_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg,
                      enum uui_state state);
// Just the label, centred on a face the caller already painted in `fill`
// -- for a button drawn in another shape (uui_button's outlined style).
void uui_button_draw_label(struct ugfx_surface *s, int x, int y, int w, int h,
                           const char *label, uint32_t fg, uint32_t fill,
                           enum uui_state state);

#endif
