#ifndef UUI_H
#define UUI_H

#include <stdint.h>
#include "ui/ugfx.h"

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

// Fills the rect with `bg` shifted for `state`, then centres `label`
// (may be NULL) in `fg`. Pressed draws a darker fill plus a 1px
// down-right nudge of the label -- the darker fill alone reads as a
// colour change rather than a press.
void uui_button_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg,
                      enum uui_state state);

// --- buttons and groups ----------------------------------------------

struct uui_button {
    int x, y, w, h;      // content-relative
    const char *label;   // not owned -- must outlive the button
    uint32_t bg, fg;
    int code;            // app-defined id, handed back on a completed click
    int pressed;         // OWNED -- driven by uui_button_group_press/_release
    int hovered;         // OWNED -- driven by uui_button_group_hover
    int disabled;
};

void uui_button_init(struct uui_button *b, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg, int code);

// Repositions without touching pressed/hovered. Separate from init()
// for a real reason inherited from the kernel version: geometry is
// font-size dependent and gets recomputed on redraws, and re-running
// init() every frame would wipe in-progress press state before it could
// ever be drawn.
void uui_button_set_geometry(struct uui_button *b, int x, int y, int w, int h);

struct uui_button_group {
    struct uui_button *buttons; // not owned -- caller's array
    int count;
};

void uui_button_group_init(struct uui_button_group *g,
                            struct uui_button *buttons, int count);

void uui_button_group_draw(const struct uui_button_group *g,
                            struct ugfx_surface *s);

// Re-hit-tests and updates every button's `pressed` flag. Returns 1 if
// which button is hot changed (so the caller knows to repaint). Dragging
// off one button onto another re-presses correctly because this
// re-tests every button every time rather than remembering one.
int uui_button_group_press(struct uui_button_group *g, int cx, int cy);

// Same for `hovered`, with no button held. Pass (-1, -1) when the
// cursor leaves -- nothing is hit there, so the highlight clears with
// no special case.
int uui_button_group_hover(struct uui_button_group *g, int cx, int cy);

// Clears whichever button was pressed and returns its `code`, or -1 if
// none was. **This is how a button commits.** A press dragged off its
// button already had `pressed` cleared by uui_button_group_press(), so
// it returns -1 here and the action correctly does not happen.
int uui_button_group_release(struct uui_button_group *g);

#endif
