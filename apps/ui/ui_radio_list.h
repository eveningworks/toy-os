#ifndef UI_RADIO_LIST_H
#define UI_RADIO_LIST_H

#include <stdint.h>

// A single-select list: one row per option, the active one marked, laid
// out in one or more columns. The mutual-exclusivity ui_checkbox.h
// deliberately doesn't have ("that's a radio-button concept, not this")
// -- this is that concept.
//
// **Added ahead of a second caller, by explicit user request**, the same
// deliberate exception ui_checkbox was (see docs/decisions.md, and note
// this project's standing rule is otherwise a second REAL caller before
// a widget is shared). Its one caller today is the Control Panel's Date
// & Time applet; every later applet that picks one-of-N -- keyboard
// layout, cursor style, theme -- is expected to use it, which is the
// argument that won.
//
// Deliberately NOT owned state: the caller keeps its own `selected`
// index and its own option strings, and this module only knows
// geometry, drawing and hit-testing. Same split as the rest of apps/ui/
// (see ui_button.h's top comment) -- and it means the selection can
// live wherever it actually belongs (for the timezone applet that's
// tz.c's own current index, not a copy this widget would have to be
// kept in sync with).

struct ui_radio_list {
    const char *const *options; // caller-owned array of `count` labels
    int count;
    int cols;         // columns to lay the rows out in (1 = a plain vertical list)
    int row_h;        // full height of one row, including its gap
    int col_w;        // full width of one column
    int marker_size;  // diameter of the selection marker, in pixels
};

// Total pixel size the list occupies, so a caller can size a window or
// a panel around it without duplicating the row/column arithmetic.
// Shared by draw()/hit() so the three can never disagree.
void ui_radio_list_size(const struct ui_radio_list *list, int *out_w, int *out_h);

// Draws every option at (x, y), with row `selected` marked. `selected`
// outside [0, count) simply marks nothing -- a caller whose underlying
// setting isn't in the list (an /etc file naming a city this build
// doesn't have, say) gets a list with no marker rather than a wrong one
// marked or an out-of-bounds read.
// `hovered` is the row under the cursor, or -1 for none -- washed one
// step via ui_state_bg(), the same vocabulary every other control uses.
// Pass -1 when the caller doesn't track hover, which is what every
// caller did before this parameter existed.
//
// Added because docs/gui-guidelines.md requires a hover state on
// anything clickable and this had none. The hit area is the whole row
// (see ui_radio_list_hit below), so the wash covers the whole row too --
// a highlight smaller than the target it describes misreports where to
// click.
void ui_radio_list_draw(const struct ui_radio_list *list, int x, int y, int selected,
                         int hovered, uint32_t bg, uint32_t fg, uint32_t accent);

// Index of the option at (px, py), or -1 if the point isn't on one.
// Hit areas are the full row rectangle (marker + label + the gap
// between them), not just the marker or the text -- a 14px circle is a
// mean click target, and the whole row reading as clickable is what
// every real settings list does.
int ui_radio_list_hit(const struct ui_radio_list *list, int x, int y, int px, int py);

#endif
