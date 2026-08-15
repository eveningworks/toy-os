#ifndef UI_PRIMITIVES_H
#define UI_PRIMITIVES_H

#include <stdint.h>

// The two lowest-level "clickable rectangle" primitives everything else
// in apps/ui/ is built on -- pulled out after three independent
// reimplementations of the same idea turned up: wm.c's title-bar
// buttons, Calculator's button grid, and Notepad's toolbar. Originally
// lived in apps/widgets.c/.h (this codebase's very first shared GUI
// helper); moved here when every other widget in that file also moved
// into its own apps/ui/ file, so apps/widgets.c/.h no longer exist --
// see docs/decisions.md for why the whole directory looks the way it
// does now.
//
// Deliberately minimal: a filled rect with an optional centered text
// label, plus a hit test. `ui_button`/`ui_textbox` (this directory)
// both wrap widget_button()/widget_hit() to add owned geometry/state on
// top -- this file itself stays a bare stateless primitive, the same
// role it's always had.

// Shared text-cursor bar width -- a thin vertical bar, not a solid
// full-cell block, used by both ui_textbox's caret and ui_scrollback's
// cursor so the two read as the same "modern insert-point" shape
// everywhere text can be edited in the GUI.
#define CURSOR_BAR_W 2

// 1 if (px, py) falls inside the x/y/w/h rect, 0 otherwise. Whatever
// coordinate space the caller's other numbers are already in (screen
// pixels for wm.c, window-content-relative for Calculator/Notepad) --
// this doesn't care, it's just a bounds check.
int widget_hit(int x, int y, int w, int h, int px, int py);

// --- natural size ------------------------------------------------------
//
// Every widget answers ONE question the same way:
//
//     void ui_<widget>_natural_size(const struct ..., int *w, int *h);
//
// **The PREFERRED MINIMUM -- the smallest size at which the widget looks
// right.** A layout is free to hand it more (a listbox filling a
// window's height, a text field filling a row's width), and every
// widget here already draws correctly into whatever rectangle it is
// given, so this is a request rather than a demand. That is what makes
// a resizable window possible at all: with an exact size, nothing grows
// to fill one.
//
// **0 means NO PREFERENCE, and callers must handle it.** A text field
// has no intrinsic width -- twenty columns is no more correct than
// fifteen -- so it writes 0 and lets the layout decide, while still
// reporting a real height that must be honoured. Writing an invented
// default instead would look tidier and be a lie: the number would
// never be the actual answer once anything stretched it.
//
// This replaced three different spellings of the same idea that had
// grown up separately -- `ui_radio_list_size()`, `widget_checkbox_width()`
// (width only) and `ui_listbox_height_for_rows()` (height only,
// parameterised). Three real callers, no shared signature; naming it
// once is consolidation, not speculation.
//
// Padding is FONT-DERIVED, never a pixel constant, so a widget sized
// this way reflows with `fontsize` like the rest of the GUI.
#define UI_PAD_X (gfx_char_w())
#define UI_PAD_Y (gfx_char_h() / 2)


// Fills the rect with `bg`, then -- if `label` is non-NULL -- centers it
// in `fg` on `bg` using the current font (gfx_char_w()/gfx_char_h(), so
// it stays correct across gfx_set_font_size() calls same as everything
// else that draws text). Pass label=NULL for an icon-only button.
//
// The interaction states every clickable control in this GUI has. See
// docs/gui-guidelines.md for what each one means and when a control is
// required to show them; this enum is that document's vocabulary in
// code, so the two can't drift into describing different things.
enum ui_state {
    UI_STATE_REST = 0,  // idle -- the control's own colours, no decoration
    UI_STATE_HOVER,     // cursor is over it, no button held
    UI_STATE_PRESSED,   // held down AND the cursor is still over it
    UI_STATE_DISABLED,  // visible, explained by its context, does nothing
};

// `base` shifted for `state`: a light wash for hover, a darker one for
// pressed, muted for disabled, and `base` itself at rest.
//
// Derived from the caller's own colour rather than read from a fixed
// palette, so it works for any control -- the title bar's red close
// button gets a lighter red on hover and a darker red pressed, with no
// special case, where the old code carried a hand-picked
// `hover_tint_close` constant precisely because it couldn't do this.
uint32_t ui_state_bg(uint32_t base, enum ui_state state);

// Fills the rect with `bg` shifted for `state`, then -- if `label` is
// non-NULL -- centers it in `fg` using the current font. Pass
// label=NULL for an icon-only button.
//
// Pressed shows as a DARKER FILL plus a 1px down-right nudge of the
// label, not the 2px inset border this used to draw. The inset border
// existed for a concrete reason that has since expired: darkening an
// arbitrary packed pixel needed colour maths this file couldn't reach,
// as the previous version of this comment explained at length.
// gfx_blend() became public for the console cursor's translucent style
// and lifted that constraint; nothing revisited this until the GUI
// guidelines asked for one flat, modern language across every control.
void widget_button_state(int x, int y, int w, int h, const char *label,
                          uint32_t bg, uint32_t fg, enum ui_state state);

// widget_button_state() with REST/PRESSED only. Kept because most
// callers genuinely have no hover state to report (nothing delivers
// them mouse motion), and `pressed ? PRESSED : REST` at every one of
// those call sites would be noise.
void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg, int pressed);

#endif
