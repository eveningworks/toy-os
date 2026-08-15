// scrollbar. Split out of uwidgets.c -- see ui/uui_scrollbar.h.
#include "ui/uui_scrollbar.h"

// ---------------------------------------------------------------------
// scrollbar
// ---------------------------------------------------------------------

// THE shared geometry. Thumb size and position depend only on the line
// counts, never on x -- computing it once here is what stops draw(),
// hit() and the drag maths disagreeing about where the thumb is.
static void sb_geometry(int y, int h, int total_lines, int visible_rows, int scroll_offset,
                         int *out_thumb_y, int *out_thumb_h, int *out_max_scroll) {
    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;

    int thumb_h = (total_lines > 0) ? h * visible_rows / total_lines : h;
    if (thumb_h < UUI_SCROLLBAR_MIN_THUMB_H) thumb_h = UUI_SCROLLBAR_MIN_THUMB_H;
    if (thumb_h > h) thumb_h = h;

    int track_range = h - thumb_h;
    int thumb_y = y;
    if (max_scroll > 0 && track_range > 0) {
        // offset 0 (pinned to newest) -> thumb at the BOTTOM;
        // offset max_scroll (oldest) -> thumb at the top.
        thumb_y = y + track_range - track_range * scroll_offset / max_scroll;
    }

    *out_thumb_y = thumb_y;
    *out_thumb_h = thumb_h;
    *out_max_scroll = max_scroll;
}

void uui_scrollbar_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                         int total_lines, int visible_rows, int scroll_offset,
                         uint32_t track_bg, uint32_t thumb_bg) {
    ugfx_fill_rect(s, x, y, w, h, track_bg);
    if (total_lines <= visible_rows) return; // it all fits -- no thumb

    int ty, th, ms;
    sb_geometry(y, h, total_lines, visible_rows, scroll_offset, &ty, &th, &ms);
    ugfx_fill_rect(s, x, ty, w, th, thumb_bg);
}

enum uui_scrollbar_zone uui_scrollbar_hit(int x, int y, int w, int h,
                                           int total_lines, int visible_rows,
                                           int scroll_offset, int px, int py) {
    if (!uui_hit(x, y, w, h, px, py)) return UUI_SB_NONE;
    if (total_lines <= visible_rows) return UUI_SB_NONE;

    int ty, th, ms;
    sb_geometry(y, h, total_lines, visible_rows, scroll_offset, &ty, &th, &ms);
    if (py < ty) return UUI_SB_ABOVE;
    if (py >= ty + th) return UUI_SB_BELOW;
    return UUI_SB_THUMB;
}

void uui_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                               int scroll_offset, int *out_thumb_y, int *out_thumb_h) {
    int ms;
    sb_geometry(y, h, total_lines, visible_rows, scroll_offset, out_thumb_y, out_thumb_h, &ms);
}

int uui_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                   int py, int grab_offset_in_thumb) {
    int ty, th, max_scroll;
    // The 0 here is arbitrary: only thumb_h and max_scroll are used, and
    // the drag itself determines the new position.
    sb_geometry(y, h, total_lines, visible_rows, 0, &ty, &th, &max_scroll);
    if (max_scroll <= 0) return 0;

    int track_range = h - th;
    if (track_range <= 0) return 0;

    int new_y = py - grab_offset_in_thumb;
    if (new_y < y) new_y = y;
    if (new_y > y + track_range) new_y = y + track_range;

    int offset = max_scroll - (new_y - y) * max_scroll / track_range;
    if (offset < 0) offset = 0;
    if (offset > max_scroll) offset = max_scroll;
    return offset;
}
