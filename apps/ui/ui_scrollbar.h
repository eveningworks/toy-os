#ifndef UI_SCROLLBAR_H
#define UI_SCROLLBAR_H

#include <stdint.h>

// A companion to ui_scrollback.h's text_scrollback (or anything else
// that can report a total-lines/visible-rows/scroll_offset triple --
// nothing here actually depends on text_scrollback's struct): draws a
// track + proportional thumb, classifies a click as landing on the
// thumb (start a drag) or the empty track above/below it (page up/
// down), and converts an in-progress drag's mouse position back into a
// scroll_offset. Originally lived in apps/widgets.c/.h; moved here (same
// content, no behavior/rename change) alongside the rest of that file's
// widgets -- see docs/decisions.md.
//
// Geometry (thumb size/position) is recomputed from the three inputs on
// every call rather than cached, so `x/y/w/h` and
// `total_lines`/`visible_rows`/`scroll_offset` must be the SAME values
// across a draw()/hit()/thumb_rect() call for them to agree with each
// other (get total_lines/visible_rows from widget_scrollback_metrics(),
// same call, right before using any of these).
//
// `scroll_offset` follows text_scrollback's convention: 0 = pinned to
// the bottom/newest (thumb at the bottom of the track), increasing
// toward `total_lines - visible_rows` = fully scrolled to the
// oldest/top (thumb at the top of the track).
#define SCROLLBAR_MIN_THUMB_H 16

// When a control that OWNS a scrollbar should show one. Lives here
// rather than with any one such control, because it is shared
// vocabulary: ui_textview and ui_listbox both have this exact policy
// and must mean the same thing by it. (It started in ui_textview.h,
// which was fine while that was the only owner; ui_listbox arriving
// made a listbox's scrollbar policy an enum from the *text view's*
// header, which reads like a dependency that isn't there.)
enum ui_scrollbar_policy {
    // Show the bar only when the content actually overflows AND the
    // control is wide enough to spare the strip. The second half is not
    // decoration: Terminal has always hidden its bar below a minimum
    // width, because a scrollbar eating a third of a narrow window is
    // worse than no scrollbar.
    UI_SCROLLBAR_AUTO = 0,
    UI_SCROLLBAR_ALWAYS, // reserve the strip even when it can't scroll
    UI_SCROLLBAR_NEVER,  // no bar; the wheel still scrolls
};

enum scrollbar_zone {
    SCROLLBAR_ZONE_NONE,  // (px, py) isn't inside the track rect at all
    SCROLLBAR_ZONE_THUMB, // landed on the thumb -- caller should start a drag
    SCROLLBAR_ZONE_ABOVE, // landed on the empty track above the thumb -- page toward older content
    SCROLLBAR_ZONE_BELOW, // landed on the empty track below the thumb -- page toward newer content
};

// Fills (x, y, w, h) with `track_bg`, then draws the proportionally-sized
// thumb in `thumb_bg`. If `total_lines <= visible_rows` (nothing to
// scroll), only the track is drawn.
// **The scrollbar is deliberately NOT an object**, unlike every other
// widget here. Its state -- how many lines there are, how many are
// visible, where the view is scrolled to -- belongs to whatever it
// scrolls: ui_listbox, ui_textview and the file picker each own that
// already. Giving the scrollbar its own copy would be two sources of
// truth for one fact, which is the bug class this file's shared
// geometry helper exists to prevent. It is a rendering helper for other
// widgets, not a layout child, so nothing ever places a bare one.
//
// That is what the `widget_` prefix means now, in what is otherwise a
// tree of `ui_<widget>_*` objects: **a stateless helper that is not a
// layout child.** widget_hit() is the other one. The prefix used to
// mean "not converted yet"; ui_checkbox becoming an object spent the
// last of that meaning, so it was worth restating as a rule rather than
// leaving as an accident.

// Preferred minimum: the strip's width, and no height preference -- a
// scrollbar is exactly as tall as whatever it scrolls. See
// ui_primitives.h. Still a free function; the struct comes when the
// checkbox and scrollbar become objects and can join a layout.
void widget_scrollbar_natural_size(int *out_w, int *out_h);

void widget_scrollbar_draw(int x, int y, int w, int h, int total_lines, int visible_rows,
                            int scroll_offset, uint32_t track_bg, uint32_t thumb_bg);

// Classifies (px, py) against the same geometry widget_scrollbar_draw()
// would compute for these arguments.
enum scrollbar_zone widget_scrollbar_hit(int x, int y, int w, int h, int total_lines,
                                          int visible_rows, int scroll_offset, int px, int py);

// Outputs the thumb's current y position and height (in the same
// coordinate space as `y`/`h`) for this geometry -- used when a click
// lands on SCROLLBAR_ZONE_THUMB, to record where within the thumb the
// grab happened (`py - *out_thumb_y`) so widget_scrollbar_offset_for_drag()
// below can keep that same relative grab point under the cursor for the
// rest of the drag.
void widget_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                                  int scroll_offset, int *out_thumb_y, int *out_thumb_h);

// Converts an in-progress drag's current mouse `py` back into a
// scroll_offset, given `grab_offset_in_thumb` (the `py - thumb_y` value
// captured via widget_scrollbar_thumb_rect() when the drag started).
// The result is already clamped to [0, total_lines - visible_rows] --
// safe to assign straight to a text_scrollback's `scroll_offset` field.
int widget_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                      int py, int grab_offset_in_thumb);

#endif
