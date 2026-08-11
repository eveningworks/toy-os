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

enum scrollbar_zone {
    SCROLLBAR_ZONE_NONE,  // (px, py) isn't inside the track rect at all
    SCROLLBAR_ZONE_THUMB, // landed on the thumb -- caller should start a drag
    SCROLLBAR_ZONE_ABOVE, // landed on the empty track above the thumb -- page toward older content
    SCROLLBAR_ZONE_BELOW, // landed on the empty track below the thumb -- page toward newer content
};

// Fills (x, y, w, h) with `track_bg`, then draws the proportionally-sized
// thumb in `thumb_bg`. If `total_lines <= visible_rows` (nothing to
// scroll), only the track is drawn.
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
