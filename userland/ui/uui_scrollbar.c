// scrollbar. Split out of uwidgets.c -- see ui/uui_scrollbar.h.
#include "ui/uui_scrollbar.h"

// ---------------------------------------------------------------------
// scrollbar
// ---------------------------------------------------------------------

// The TRACK -- the part of the bar the thumb can occupy. With arrows
// on, it is the strip between them. Everything below works in track
// coordinates, so the arrows cost one substitution rather than a
// special case in each function.
static void sb_track(int y, int h, int bar_w, unsigned flags, int *ty, int *th) {
    if (flags & UUI_SCROLLBAR_ARROWS) {
        int a = uui_scrollbar_arrow_h(bar_w);
        if (2 * a < h) { *ty = y + a; *th = h - 2 * a; return; }
        // Too short for arrows AND a track: the track wins, since a
        // scrollbar with no room to scroll in is useless.
    }
    *ty = y;
    *th = h;
}

// How far the thumb is inset from each side of the strip. Derived from
// the bar's width rather than fixed at 1px: the gutter is what reads as
// "a capsule in a groove" instead of "a block filling it", and a single
// pixel of it disappears as the bar gets wider. Kept to whole pixels and
// never more than 2, so a narrow bar still has a thumb worth grabbing.
int uui_scrollbar_thumb_inset(int w) {
    int inset = 1 + w / 12;
    if (inset > 2) inset = 2;
    if (w <= 4) inset = 0;
    return inset;
}

// A rounded rectangle -- the capsule shape a modern scrollbar thumb
// has. Just the four corner pixels trimmed: at 8-14px wide, that is the
// whole difference between "capsule" and "block", and it needs no
// anti-aliasing or new drawing machinery to read correctly.
static void fill_capsule(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) {
    if (w <= 2 || h <= 2) { ugfx_fill_rect(s, x, y, w, h, c); return; }
    ugfx_fill_rect(s, x, y + 1, w, h - 2, c);       // the body
    ugfx_fill_rect(s, x + 1, y, w - 2, 1, c);       // top, inset
    ugfx_fill_rect(s, x + 1, y + h - 1, w - 2, 1, c); // bottom, inset
}

// The same capsule lying down: the trimmed corners move to the ends, so
// a horizontal thumb reads as a capsule rather than as a bar with two
// notches bitten out of its long edges.
static void fill_capsule_h(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) {
    if (w <= 2 || h <= 2) { ugfx_fill_rect(s, x, y, w, h, c); return; }
    ugfx_fill_rect(s, x + 1, y, w - 2, h, c);
    ugfx_fill_rect(s, x, y + 1, 1, h - 2, c);
    ugfx_fill_rect(s, x + w - 1, y + 1, 1, h - 2, c);
}

// A small solid triangle, for a stepper arrow. `dir` is -1 for up.
static void fill_arrow(struct ugfx_surface *s, int x, int y, int w, int h,
                        int dir, uint32_t c) {
    int cx = x + w / 2;
    int rows = h / 2;
    if (rows < 2) rows = 2;
    int top = y + (h - rows) / 2;
    for (int i = 0; i < rows; i++) {
        int half = (dir < 0) ? i : rows - 1 - i;
        ugfx_fill_rect(s, cx - half, top + i, 2 * half + 1, 1, c);
    }
}

// The same triangle pointing left or right: columns instead of rows.
static void fill_arrow_h(struct ugfx_surface *s, int x, int y, int w, int h,
                          int dir, uint32_t c) {
    int cy = y + h / 2;
    int cols = w / 2;
    if (cols < 2) cols = 2;
    int left = x + (w - cols) / 2;
    for (int i = 0; i < cols; i++) {
        int half = (dir < 0) ? i : cols - 1 - i;
        ugfx_fill_rect(s, left + i, cy - half, 1, 2 * half + 1, c);
    }
}

// THE shared geometry. Thumb size and position depend only on the line
// counts, never on x -- computing it once here is what stops draw(),
// hit() and the drag maths disagreeing about where the thumb is.
static void sb_geometry(int y, int h, int total_lines, int visible_rows, int scroll_offset,
                         int *out_thumb_y, int *out_thumb_h, int *out_max_scroll,
                         unsigned flags) {
    int max_scroll = total_lines > visible_rows ? total_lines - visible_rows : 0;

    int thumb_h = (total_lines > 0) ? h * visible_rows / total_lines : h;
    if (thumb_h < UUI_SCROLLBAR_MIN_THUMB_H) thumb_h = UUI_SCROLLBAR_MIN_THUMB_H;
    if (thumb_h > h) thumb_h = h;

    int track_range = h - thumb_h;
    int thumb_y = y;
    if (max_scroll > 0 && track_range > 0) {
        // THE TWO AXES MEASURE FROM OPPOSITE ENDS (ui/uui_scrollbar.h).
        // Vertically, offset 0 is pinned to the NEWEST text, so the
        // thumb sits at the bottom; horizontally, 0 is the left margin.
        thumb_y = (flags & UUI_SCROLLBAR_HORIZ)
                    ? y + track_range * scroll_offset / max_scroll
                    : y + track_range - track_range * scroll_offset / max_scroll;
    }

    *out_thumb_y = thumb_y;
    *out_thumb_h = thumb_h;
    *out_max_scroll = max_scroll;
}

void uui_scrollbar_natural_size(int *out_w, int *out_h) {
    // char_w + 6, not + 4: the narrower strip left a 6px thumb that was
    // genuinely hard to hit with a mouse, which is what raised this.
    // Still font-derived, so the bar stays proportional at every font
    // size -- a fixed pixel width would be right at exactly one of them.
    if (out_w) *out_w = ugfx_char_w() + 6;
    if (out_h) *out_h = 0; // no preference: as tall as its content area
}

void uui_scrollbar_draw(struct ugfx_surface *s, int x, int y, int w, int h,
                         int total_lines, int visible_rows, int scroll_offset,
                         uint32_t track_bg, uint32_t thumb_bg, unsigned flags) {
    // The track is drawn whether or not there is anything to scroll: a
    // visible groove tells you the scrollbar is THERE before you go
    // looking for it, which matters at this font size. (The quieter
    // near-invisible style needs hover-to-expand to compensate, and
    // this widget deliberately keeps no hover state of its own.)
    ugfx_fill_rect(s, x, y, w, h, track_bg);

    // ONE implementation, two axes: `pos`/`len` are the scrolled axis
    // and `thick` the other, so everything below is written once.
    int horiz = (flags & UUI_SCROLLBAR_HORIZ) != 0;
    int pos = horiz ? x : y, len = horiz ? w : h, thick = horiz ? h : w;

    int tky, tkh;
    sb_track(pos, len, thick, flags, &tky, &tkh);

    if (flags & UUI_SCROLLBAR_ARROWS) {
        int a = uui_scrollbar_arrow_h(thick);
        if (tkh < len) { // arrows actually fitted
            if (horiz) {
                fill_arrow_h(s, x, y, a, h, -1, thumb_bg);
                fill_arrow_h(s, x + w - a, y, a, h, +1, thumb_bg);
            } else {
                fill_arrow(s, x, y, w, a, -1, thumb_bg);
                fill_arrow(s, x, y + h - a, w, a, +1, thumb_bg);
            }
        }
    }

    if (total_lines <= visible_rows) return; // it all fits -- no thumb

    int ty, th, ms;
    sb_geometry(tky, tkh, total_lines, visible_rows, scroll_offset, &ty, &th, &ms, flags);
    // Inset each side so the track shows around the capsule -- that gap
    // is most of what reads as "modern" rather than "a grey block
    // filling a groove". Width-derived, so a wider bar gets a wider
    // gutter instead of a fatter block; see uui_scrollbar_thumb_inset().
    int in = uui_scrollbar_thumb_inset(thick);
    if (horiz) fill_capsule_h(s, ty, y + in, th, h - 2 * in, thumb_bg);
    else       fill_capsule(s, x + in, ty, w - 2 * in, th, thumb_bg);
}

enum uui_scrollbar_zone uui_scrollbar_hit(int x, int y, int w, int h,
                                           int total_lines, int visible_rows,
                                           int scroll_offset, int px, int py,
                                           unsigned flags) {
    if (!uui_hit(x, y, w, h, px, py)) return UUI_SB_NONE;

    int horiz = (flags & UUI_SCROLLBAR_HORIZ) != 0;
    int pos = horiz ? x : y, len = horiz ? w : h, thick = horiz ? h : w;
    int p = horiz ? px : py;

    int tky, tkh;
    sb_track(pos, len, thick, flags, &tky, &tkh);
    // Arrows answer even when everything fits, so a click on one is
    // never mistaken for a click on the track behind it.
    if (tkh < len) {
        if (p < tky) return UUI_SB_UP;
        if (p >= tky + tkh) return UUI_SB_DOWN;
    }
    if (total_lines <= visible_rows) return UUI_SB_NONE;

    int ty, th, ms;
    sb_geometry(tky, tkh, total_lines, visible_rows, scroll_offset, &ty, &th, &ms, flags);
    if (p < ty) return UUI_SB_ABOVE;
    if (p >= ty + th) return UUI_SB_BELOW;
    return UUI_SB_THUMB;
}

void uui_scrollbar_thumb_rect(int y, int h, int total_lines, int visible_rows,
                               int scroll_offset, int *out_thumb_y, int *out_thumb_h,
                               int bar_w, unsigned flags) {
    int tky, tkh, ms;
    sb_track(y, h, bar_w, flags, &tky, &tkh);
    sb_geometry(tky, tkh, total_lines, visible_rows, scroll_offset,
                 out_thumb_y, out_thumb_h, &ms, flags);
}

int uui_scrollbar_offset_for_drag(int y, int h, int total_lines, int visible_rows,
                                   int py, int grab_offset_in_thumb,
                                   int bar_w, unsigned flags) {
    int ty, th, max_scroll;
    sb_track(y, h, bar_w, flags, &y, &h); // drag in TRACK coordinates
    // The 0 here is arbitrary: only thumb_h and max_scroll are used, and
    // the drag itself determines the new position.
    sb_geometry(y, h, total_lines, visible_rows, 0, &ty, &th, &max_scroll, flags);
    if (max_scroll <= 0) return 0;

    int track_range = h - th;
    if (track_range <= 0) return 0;

    int new_y = py - grab_offset_in_thumb;
    if (new_y < y) new_y = y;
    if (new_y > y + track_range) new_y = y + track_range;

    // Mirrors sb_geometry's two directions: the drag must invert the
    // same mapping the thumb was drawn with, or it runs backwards.
    int offset = (flags & UUI_SCROLLBAR_HORIZ)
                   ? (new_y - y) * max_scroll / track_range
                   : max_scroll - (new_y - y) * max_scroll / track_range;
    if (offset < 0) offset = 0;
    if (offset > max_scroll) offset = max_scroll;
    return offset;
}
