#ifndef UI_DROPDOWN_H
#define UI_DROPDOWN_H

#include <stdint.h>
#include "ui_listbox.h"

// A dropdown (combo box): a closed box showing the current value, which
// opens a popup list to change it.
//
// It COMPOSES ui_listbox for that popup rather than reimplementing a
// list -- the same layering ui_textview uses over text_scrollback +
// ui_scrollbar, and ui_button_group over ui_button. A Windows combo box
// genuinely is a button plus a popup listbox, so the scrolling,
// keyboard navigation, hover and selection all come from the listbox for
// free, and there is exactly one implementation of them to get right.
// `dd.list` is a real ui_listbox and its config fields (row_h, policy,
// wheel_rows, colours) work as documented there.
//
// ---------------------------------------------------------------------
// The popup, and the one rule an app has to follow
// ---------------------------------------------------------------------
//
// Drawing here is immediate-mode: z-order IS call order. A popup must
// therefore be drawn AFTER every other widget in the window, or whatever
// is drawn later lands on top of it. That cannot be hidden inside
// ui_dropdown_draw() without a deferred-draw mechanism this GUI does not
// have, so it is explicit instead:
//
//   on_draw:  ...every other widget...
//             ui_dropdown_draw(&dd, ox, oy);        // the closed box
//             ...
//             ui_dropdown_draw_popup(&dd, ox, oy);  // LAST, after everything
//
// `ui_dropdown_draw_popup()` is a no-op while closed, so it is always
// safe to call unconditionally, and an app with several dropdowns can
// call them all in a row at the end.
//
// Input is the mirror image: the popup is on top, so it gets FIRST
// refusal. Forward presses/hover/keys/wheel to the dropdown BEFORE the
// widgets underneath it, and stop if it consumed the event.
//
// **The popup cannot escape the window.** wm_render_frame() clips each
// app's on_draw() to its own content rect (docs/gui-guidelines.md), so
// unlike a real Windows combo the list cannot spill past the window
// edge. Instead it FLIPS ABOVE the box when there isn't room below, and
// shrinks to the room available when there isn't room either way -- the
// scrollbar it inherits from ui_listbox is what makes that acceptable
// rather than a truncation.
//
// **Added ahead of a second caller, by explicit user request** -- the
// same deliberate exception ui_checkbox and ui_radio_list were (see
// docs/decisions.md). Its caller today is UI Demo.
//
// ---------------------------------------------------------------------
// Behaviour (Windows' combo box, minus what this GUI does differently)
// ---------------------------------------------------------------------
//
//   - Clicking the closed box opens the popup, with the current value
//     selected and scrolled into view.
//   - Clicking a row commits it and closes. Commit is on RELEASE over
//     the row, so a press dragged off and released changes nothing.
//   - Clicking anywhere outside an open popup closes it and leaves the
//     value alone.
//   - Arrows/Home/End/PageUp/PageDown move the highlight while open;
//     Enter commits it, Esc restores the value the dropdown had when it
//     was opened.
//   - While CLOSED, arrows change the value directly -- Windows does
//     this too, and it is why a combo can be operated without ever
//     seeing the list.
//   - The wheel scrolls an open popup without changing the value. While
//     closed it is ignored, deliberately: a wheel over a closed combo
//     silently changing a setting the user is merely scrolling past is
//     a well-known way to lose data, and several real toolkits have
//     walked it back.

struct ui_dropdown {
    // Content-relative geometry of the CLOSED box -- same convention as
    // every other widget here. The popup positions itself against this.
    int x, y, w, h;

    struct ui_listbox list; // the popup's list -- see ui_listbox.h

    int open;
    int max_rows;   // most rows the popup shows before it scrolls
    int disabled;   // 1 = drawn dimmed, ignores every input
    int hovered;    // OWNED: 1 when the cursor is over the closed box
    int pressed;    // OWNED: 1 while the box is held

    // Value to restore if the popup is dismissed with Esc -- captured
    // when it opens. Not a general undo: only Esc uses it.
    int value_on_open;

    uint32_t bg, fg, border;

    // Popup geometry resolved by the last draw/open, in content-relative
    // coordinates. Cached because hit-testing (which runs from event
    // handlers, with no chance to recompute layout) has to agree exactly
    // with what was drawn -- one geometry, shared, which is the rule
    // docs/gui-guidelines.md states after the Control Panel shipped a
    // grid that drew perfectly and opened nothing.
    int popup_x, popup_y, popup_w, popup_h;
};

// Geometry, items and colours. `selected` may be -1 for "no value yet".
// Popup colours are derived from the box's, so a caller that wants them
// different can set dd.list's colour fields afterwards.
void ui_dropdown_init(struct ui_dropdown *dd, int x, int y, int w, int h,
                       const char *const *items, int count, int selected,
                       uint32_t bg, uint32_t fg, uint32_t border,
                       uint32_t sel_bg, uint32_t sel_fg,
                       uint32_t track_bg, uint32_t thumb_bg);

// Updates the CLOSED box's x/y/w/h only. The popup is re-laid-out
// against it on the next draw, so this is safe to call every frame.
void ui_dropdown_set_geometry(struct ui_dropdown *dd, int x, int y, int w, int h);

// Current value: the selected index, or -1. `ui_dropdown_selected_text()`
// returns the matching label, or NULL when nothing is selected.
int ui_dropdown_selected(const struct ui_dropdown *dd);
const char *ui_dropdown_selected_text(const struct ui_dropdown *dd);
void ui_dropdown_set_selected(struct ui_dropdown *dd, int index);

// Opens/closes the popup directly -- what a keyboard shortcut or a
// programmatic reset would use. Opening captures the Esc-restore value.
void ui_dropdown_open(struct ui_dropdown *dd);
void ui_dropdown_close(struct ui_dropdown *dd);

// Draws the closed box (value + arrow). Draws NOTHING of the popup --
// see the header comment above for why that is a separate call.
void ui_dropdown_draw(struct ui_dropdown *dd, int origin_x, int origin_y);

// Draws the popup if open, and nothing at all if not. **Must be called
// after every other widget in the window**, or they will draw over it.
void ui_dropdown_draw_popup(struct ui_dropdown *dd, int origin_x, int origin_y);

// 1 if (cx, cy) is on the closed box.
int ui_dropdown_hit(const struct ui_dropdown *dd, int cx, int cy);

// 1 if (cx, cy) is inside the OPEN popup (always 0 while closed). An app
// that needs to know whether the dropdown will swallow a press before
// forwarding it can ask this; forwarding and checking the return value
// is usually simpler.
int ui_dropdown_popup_hit(const struct ui_dropdown *dd, int cx, int cy);

// --- input; each returns non-zero when it consumed the event ---------
//
// Forward these BEFORE the widgets underneath, since an open popup is
// drawn on top of them.

// Returns 1 when the hovered element CHANGED -- the on_hover contract
// (docs/gui-guidelines.md), same as ui_listbox_hover().
int ui_dropdown_hover(struct ui_dropdown *dd, int cx, int cy);

int ui_dropdown_press(struct ui_dropdown *dd, int cx, int cy);

// Commits whatever the press armed. Returns the newly selected index if
// the value CHANGED, and -1 otherwise -- including when the popup merely
// opened, closed, or was dismissed. So `if (ui_dropdown_release(&dd) >= 0)`
// is exactly "the user picked a new value".
int ui_dropdown_release(struct ui_dropdown *dd);

int ui_dropdown_wheel(struct ui_dropdown *dd, int delta);

// Arrows/Home/End/PageUp/PageDown, plus Enter (commit) and Esc (restore
// and close) while open. Returns 1 if the key was consumed.
int ui_dropdown_key(struct ui_dropdown *dd, int key);

// Joins a ui_focus ring (see ui_focus.h). Its hit test covers the OPEN
// POPUP too, so clicking a popup row keeps focus here rather than moving
// it to whatever widget the popup is covering; and losing focus closes
// the popup, since a list left hanging over other widgets would keep
// swallowing clicks aimed at them.
extern const struct ui_focus_ops ui_dropdown_focus_ops;

#endif
