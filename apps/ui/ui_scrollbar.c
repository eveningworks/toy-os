// See ui_scrollbar.h for the design writeup.
#include "ui_scrollbar.h"
#include "ui_primitives.h" // widget_hit()
#include "kapi.h"

// Shared geometry: thumb height/position depend only on
// total_lines/visible_rows/scroll_offset (not on x, which callers vary
// to place the bar at the right edge of their content) -- computing it
// once here means draw()/hit()/thumb_rect()/offset_for_drag() can never
// disagree with each other about where the thumb is.
static void scrollbar_geometry(int y, int h, int total_lines, int visible_rows, int scroll_offset,
                                int *out_thumb_y, int *out_thumb_h, int *out_max_scroll) {
    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;

    int thumb_h = (total_lines > 0) ? h * visible_rows / total_lines : h;
    if (thumb_h < SCROLLBAR_MIN_THUMB_H) thumb_h = SCROLLBAR_MIN_THUMB_H;
    if (thumb_h > h) thumb_h = h;

    int track_range = h - thumb_h;
    int thumb_y = y;
    if (max_scroll > 0 && track_range > 0) {
        // scroll_offset == 0 (pinned to newest) -> thumb at the bottom;
        // scroll_offset == max_scroll (oldest) -> thumb at the top.
        thumb_y = y + track_range - track_range * scroll_offset / max_scroll;
    }

    *out_thumb_y = thumb_y;
    *out_thumb_h = thumb_h;
    *out_max_scroll = max_scroll;
}

void widget_scrollbar_draw(int x, int y, int w, int h, int total_lines, int visible_rows,
                            int scroll_offset, uint32_t track_bg, uint32_t thumb_bg) {
    gfx_fill_rect(x, y, w, h, track_bg);
    if (total_lines <= visible_rows) return; // everything fits -- no thumb to show

    int thumb_y, thumb_h, max_scroll;
    scrollbar_geometry(y, h, total_lines, visible_rows, scroll_offset, &thumb_y, &thumb_h, &max_scroll);
    gfx_fill_rect(x, thumb_y, w, thumb_h, thumb_bg);
}

enum scrollbar_zone widget_scrollbar_hit(int x, int y, int w, int h, int total_lines,
                                          int visible_rows, int scroll_offset, int px, int py) {
    if (!widget_hit(x, y, w, h, px, py)) return SCROLLBAR_ZONE_NONE;
    if (total_lines <= visible_rows) return SCROLLBAR_ZONE_NONE; // nothing to scroll -- no thumb, no paging

    int thumb_y, thumb_h, max_scroll;
    scrollbar_geometry(y, h, total_lines, visible_rows, scroll_offset, &thumb_y, &thumb_h, &max_scroll);

    if (py < thumb_y) return SCROLLBAR_ZONE_ABOVE;
    if (py >= thumb_y + thumb_h) return SCROLLBAR_ZONE_BELOW;
    return SCROLLBAR_ZONE_THUMB;
}

void widget_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                                  int scroll_offset, int *out_thumb_y, int *out_thumb_h) {
    int max_scroll;
    scrollbar_geometry(y, h, total_lines, visible_rows, scroll_offset, out_thumb_y, out_thumb_h, &max_scroll);
}

int widget_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                      int py, int grab_offset_in_thumb) {
    int thumb_y, thumb_h, max_scroll;
    // scroll_offset=0 here is arbitrary (only thumb_h/max_scroll are
    // used below -- thumb_y from this call is irrelevant, the drag
    // itself is what determines the thumb's position now).
    scrollbar_geometry(y, h, total_lines, visible_rows, 0, &thumb_y, &thumb_h, &max_scroll);
    if (max_scroll <= 0) return 0;

    int track_range = h - thumb_h;
    if (track_range <= 0) return 0;

    int new_thumb_y = py - grab_offset_in_thumb;
    if (new_thumb_y < y) new_thumb_y = y;
    if (new_thumb_y > y + track_range) new_thumb_y = y + track_range;

    int offset = max_scroll - (new_thumb_y - y) * max_scroll / track_range;
    if (offset < 0) offset = 0;
    if (offset > max_scroll) offset = max_scroll;
    return offset;
}
