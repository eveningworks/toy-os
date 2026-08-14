// See ui_listbox.h for the design writeup -- in particular why the
// scrolling, hit-testing and selection all belong to this control
// rather than to each app that shows a list.
#include "ui_listbox.h"
#include "ui_focus.h"
#include "kapi.h"

#define DEFAULT_WHEEL_ROWS 3
// Matched to ui_textview's bar, deliberately: two scrollbars of
// different widths in one window read as a mistake, and both are
// font-derived for the same reason (a constant is correct at exactly
// one font size).
#define DEFAULT_BAR_W (gfx_char_w() + 4)
// Below this much room for labels, AUTO hides the bar entirely -- the
// same rule and the same ratio ui_textview uses.
#define DEFAULT_MIN_ROW_W (DEFAULT_BAR_W * 3)
// Vertical breathing room inside a row, on top of the glyph height. A
// row exactly gfx_char_h() tall has the glyph's own background painting
// over the row boundary -- docs/gui-guidelines.md's "budget both axes".
#define ROW_PAD_Y 4

void ui_listbox_init(struct ui_listbox *lb, int x, int y, int w, int h,
                      const char *const *items, int count,
                      uint32_t bg, uint32_t fg, uint32_t sel_bg, uint32_t sel_fg,
                      uint32_t track_bg, uint32_t thumb_bg) {
    lb->x = x; lb->y = y; lb->w = w; lb->h = h;
    lb->items = items;
    lb->count = count;
    lb->selected = -1;
    lb->hovered = -1;
    lb->top = 0;
    lb->policy = UI_SCROLLBAR_AUTO;
    lb->bar_w = DEFAULT_BAR_W;
    lb->row_h = 0; // 0 = derive from the font, see ui_listbox_row_h()
    lb->pad_x = gfx_char_w() / 2;
    lb->wheel_rows = DEFAULT_WHEEL_ROWS;
    lb->disabled = 0;
    lb->bg = bg;
    lb->fg = fg;
    lb->sel_bg = sel_bg;
    lb->sel_fg = sel_fg;
    lb->track_bg = track_bg;
    lb->thumb_bg = thumb_bg;
    lb->armed = -1;
    lb->pressing = 0;
    lb->bar_press = 0;
    lb->thumb_grab = -1;
}

void ui_listbox_set_geometry(struct ui_listbox *lb, int x, int y, int w, int h) {
    lb->x = x; lb->y = y; lb->w = w; lb->h = h;
}

int ui_listbox_row_h(const struct ui_listbox *lb) {
    return lb->row_h > 0 ? lb->row_h : gfx_char_h() + ROW_PAD_Y;
}

int ui_listbox_visible_rows(const struct ui_listbox *lb) {
    int rh = ui_listbox_row_h(lb);
    if (rh <= 0) return 0;
    int rows = lb->h / rh; // whole rows only -- a half-drawn row is worse than a gap
    return rows > 0 ? rows : 0;
}

int ui_listbox_height_for_rows(const struct ui_listbox *lb, int rows) {
    return rows * ui_listbox_row_h(lb);
}

int ui_listbox_scrollbar_visible(const struct ui_listbox *lb) {
    if (lb->policy == UI_SCROLLBAR_NEVER) return 0;
    if (lb->w - lb->bar_w < DEFAULT_MIN_ROW_W) return 0; // too narrow to be worth the strip
    if (lb->policy == UI_SCROLLBAR_ALWAYS) return 1;
    return lb->count > ui_listbox_visible_rows(lb);
}

// Width available to row labels -- the control's width minus the
// scrollbar strip when one is showing. Same role ui_textview_text_w()
// plays there, and for the same reason: computing it any other way is
// how a row's label ends up drawn under its own scrollbar.
static int rows_w(const struct ui_listbox *lb) {
    int w = lb->w;
    if (ui_listbox_scrollbar_visible(lb)) w -= lb->bar_w;
    return w > 0 ? w : 0;
}

// Largest legal `top`: enough to show the last item at the bottom of the
// view, never negative (a list shorter than its box doesn't scroll).
static int max_top(const struct ui_listbox *lb) {
    int m = lb->count - ui_listbox_visible_rows(lb);
    return m > 0 ? m : 0;
}

static void clamp_top(struct ui_listbox *lb) {
    int m = max_top(lb);
    if (lb->top > m) lb->top = m;
    if (lb->top < 0) lb->top = 0;
}

// ui_scrollbar.h counts from the BOTTOM (0 = pinned to the newest line),
// because it grew up alongside a terminal scrollback where that is the
// useful anchor. A list counts from the top. This is the ONE place the
// two conventions meet: everything above and below works in list
// coordinates, and only the four widget_scrollbar_* calls see the
// flipped value. Converting here rather than changing ui_scrollbar keeps
// its three existing callers (Terminal, Notepad, ui_textview) untouched.
static int listbox_bar_offset(const struct ui_listbox *lb) {
    return max_top(lb) - lb->top;
}

// ...and back again, for the value a thumb drag produces.
static int listbox_top_from_bar(const struct ui_listbox *lb, int bar_offset) {
    int top = max_top(lb) - bar_offset;
    if (top < 0) top = 0;
    if (top > max_top(lb)) top = max_top(lb);
    return top;
}

void ui_listbox_set_items(struct ui_listbox *lb, const char *const *items, int count) {
    lb->items = items;
    lb->count = count < 0 ? 0 : count;
    // A stale index outliving the array it pointed into is how a
    // just-refiltered list reads one item past its end -- clamp both,
    // and drop the selection entirely if there is nothing left to select.
    if (lb->selected >= lb->count) lb->selected = lb->count > 0 ? lb->count - 1 : -1;
    if (lb->hovered >= lb->count) lb->hovered = -1;
    lb->armed = -1;
    clamp_top(lb);
}

// Scrolls the minimum distance that brings `index` fully into view --
// nothing at all when it is already visible, so arrowing within the
// visible rows doesn't jump the view around.
static void scroll_into_view(struct ui_listbox *lb, int index) {
    if (index < 0) return;
    int rows = ui_listbox_visible_rows(lb);
    if (rows <= 0) return;
    if (index < lb->top) lb->top = index;
    else if (index >= lb->top + rows) lb->top = index - rows + 1;
    clamp_top(lb);
}

void ui_listbox_set_selected(struct ui_listbox *lb, int index) {
    // Out of range marks nothing rather than being corrected to a real
    // row -- ui_radio_list's rule, for the same reason: a caller whose
    // stored setting isn't in this list should see no selection, not a
    // wrong one silently chosen for it.
    lb->selected = (index >= 0 && index < lb->count) ? index : -1;
    scroll_into_view(lb, lb->selected);
}

int ui_listbox_hit(const struct ui_listbox *lb, int cx, int cy) {
    return widget_hit(lb->x, lb->y, lb->w, lb->h, cx, cy);
}

int ui_listbox_row_at(const struct ui_listbox *lb, int cx, int cy) {
    if (!widget_hit(lb->x, lb->y, rows_w(lb), lb->h, cx, cy)) return -1;
    int rh = ui_listbox_row_h(lb);
    if (rh <= 0) return -1;
    int row = lb->top + (cy - lb->y) / rh;
    // The view can legitimately have blank space below the last item
    // (a short list, or a box that isn't a whole number of rows tall):
    // a click there is not a click on the last item.
    if (row < 0 || row >= lb->count) return -1;
    if (row >= lb->top + ui_listbox_visible_rows(lb)) return -1;
    return row;
}

// Where the scrollbar strip sits, when one is showing.
static void bar_rect(const struct ui_listbox *lb, int *bx, int *bw) {
    *bx = lb->x + lb->w - lb->bar_w;
    *bw = lb->bar_w;
}

void ui_listbox_draw(struct ui_listbox *lb, int origin_x, int origin_y) {
    int ax = origin_x + lb->x, ay = origin_y + lb->y;
    int rw = rows_w(lb);
    int rh = ui_listbox_row_h(lb);
    int rows = ui_listbox_visible_rows(lb);

    clamp_top(lb);
    gfx_fill_rect(ax, ay, lb->w, lb->h, lb->bg);

    for (int i = 0; i < rows; i++) {
        int idx = lb->top + i;
        if (idx >= lb->count) break;
        int ry = ay + i * rh;

        // Stop at the last row that FULLY fits. gfx_draw_string_clipped()
        // below bounds the width and has no notion of a row limit, so
        // without this a final row would be drawn sliced through the
        // middle of its glyphs -- docs/gui-guidelines.md's other half of
        // the clipping rule.
        if (ry + rh > ay + lb->h) break;

        // Selection is the strong state and owns the row's colours;
        // hover is a wash over whatever the row already is, so a hovered
        // selected row still reads as selected. Both go through
        // ui_state_bg() rather than hand-picked tints, which is also what
        // makes hover DARKEN on this near-white theme instead of
        // lightening into invisibility (see docs/gui-guidelines.md).
        int is_sel = (idx == lb->selected);
        uint32_t row_bg = is_sel ? lb->sel_bg : lb->bg;
        uint32_t row_fg = is_sel ? lb->sel_fg : lb->fg;

        enum ui_state state = UI_STATE_REST;
        if (lb->disabled) state = UI_STATE_DISABLED;
        else if (idx == lb->armed) state = UI_STATE_PRESSED;
        else if (idx == lb->hovered) state = UI_STATE_HOVER;

        if (state != UI_STATE_REST) row_bg = ui_state_bg(row_bg, state);
        if (lb->disabled) row_fg = ui_state_bg(row_fg, UI_STATE_DISABLED);

        gfx_fill_rect(ax, ry, rw, rh, row_bg);
        gfx_draw_string_clipped(ax + lb->pad_x, ry + (rh - gfx_char_h()) / 2,
                                 rw - lb->pad_x * 2,
                                 lb->items[idx] ? lb->items[idx] : "",
                                 row_fg, row_bg);
    }

    if (ui_listbox_scrollbar_visible(lb)) {
        int bx, bw;
        bar_rect(lb, &bx, &bw);
        widget_scrollbar_draw(origin_x + bx, ay, bw, lb->h,
                               lb->count, rows, listbox_bar_offset(lb),
                               lb->track_bg, lb->thumb_bg);
    }
}

int ui_listbox_hover(struct ui_listbox *lb, int cx, int cy) {
    // A press owns the feedback while one is in progress, and hover is
    // suppressed entirely then -- the WM already does that globally, but
    // an armed row would otherwise also light up as hovered here.
    int now = (lb->disabled || lb->pressing) ? -1 : ui_listbox_row_at(lb, cx, cy);
    if (now == lb->hovered) return 0; // unchanged -- returning 1 every tick repaints forever
    lb->hovered = now;
    return 1;
}

int ui_listbox_press(struct ui_listbox *lb, int cx, int cy) {
    if (lb->disabled) return 0;
    int rows = ui_listbox_visible_rows(lb);
    lb->hovered = -1; // the press visual owns the feedback while held

    // A thumb drag already in progress owns every subsequent tick, and
    // is checked before any hit-testing: the cursor is routinely dragged
    // well outside the control while scrolling, and losing the drag
    // there is exactly the bug this ordering prevents.
    if (lb->thumb_grab >= 0) {
        int off = widget_scrollbar_offset_for_drag(lb->y, lb->h, lb->count, rows,
                                                    cy, lb->thumb_grab);
        lb->top = listbox_top_from_bar(lb, off);
        return 1;
    }
    // Likewise a track-paging press: it acted once, on the tick it
    // started, and every later tick of the same press is a no-op rather
    // than another page.
    if (lb->bar_press) return 1;

    // Classify ONCE, on the first tick. Without this, dragging a press
    // that started on a row sideways onto the scrollbar would be read as
    // a fresh scrollbar press and start paging mid-gesture.
    if (!lb->pressing) {
        lb->pressing = 1;
        if (ui_listbox_scrollbar_visible(lb)) {
            int bx, bw;
            bar_rect(lb, &bx, &bw);
            if (cx >= bx && ui_listbox_hit(lb, cx, cy)) {
                lb->bar_press = 1; // selects nothing on release, whatever happens next
                enum scrollbar_zone zone =
                    widget_scrollbar_hit(bx, lb->y, bw, lb->h, lb->count, rows,
                                          listbox_bar_offset(lb), cx, cy);
                if (zone == SCROLLBAR_ZONE_THUMB) {
                    int ty, th;
                    widget_scrollbar_thumb_rect(lb->y, lb->h, lb->count, rows,
                                                 listbox_bar_offset(lb), &ty, &th);
                    lb->thumb_grab = cy - ty; // keep the grabbed point under the cursor
                    lb->bar_press = 0;        // the thumb-drag path owns it now
                } else if (zone == SCROLLBAR_ZONE_ABOVE || zone == SCROLLBAR_ZONE_BELOW) {
                    lb->top += (zone == SCROLLBAR_ZONE_ABOVE) ? -rows : rows;
                    clamp_top(lb);
                }
                // SCROLLBAR_ZONE_NONE (nothing to scroll) still counts
                // as a bar press and is swallowed: letting it fall
                // through would arm whatever is behind the strip.
                return 1;
            }
        }
    }

    if (!ui_listbox_hit(lb, cx, cy)) {
        // Dragged off the control: nothing is armed, so a release here
        // commits nothing. Dropped to REST rather than hover --
        // something about to be cancelled must not look like it is still
        // being interacted with (docs/gui-guidelines.md). Dragging back
        // on re-arms, which is what ui_button_group does too.
        lb->armed = -1;
        return 0;
    }

    // Recomputed every tick: the armed row follows the cursor down the
    // list, matching Windows' listbox drag-select. Still commits only on
    // release, so the gesture stays cancellable.
    lb->armed = ui_listbox_row_at(lb, cx, cy);
    return 1;
}

int ui_listbox_release(struct ui_listbox *lb) {
    int armed = lb->armed;
    int was_bar = lb->bar_press || lb->thumb_grab >= 0;
    lb->armed = -1;
    lb->pressing = 0;
    lb->bar_press = 0;
    lb->thumb_grab = -1;
    if (lb->disabled) return -1;
    if (was_bar) return -1;   // a scrollbar gesture selects nothing
    if (armed < 0) return -1; // nothing armed: never was, or dragged off

    lb->selected = armed;
    scroll_into_view(lb, lb->selected);
    return lb->selected;
}

int ui_listbox_wheel(struct ui_listbox *lb, int delta) {
    if (lb->disabled || delta == 0) return 0;
    if (lb->count <= ui_listbox_visible_rows(lb)) return 0; // nothing to scroll
    // The SELECTION deliberately does not move: a user looking further
    // down a list has not changed the value they already picked. Windows
    // behaves this way and people rely on it.
    lb->top -= delta * lb->wheel_rows;
    clamp_top(lb);
    return 1;
}

int ui_listbox_key(struct ui_listbox *lb, int key) {
    if (lb->disabled || lb->count <= 0) return 0;

    int rows = ui_listbox_visible_rows(lb);
    if (rows < 1) rows = 1;
    int sel = lb->selected;
    int next = sel;

    switch (key) {
    // From nothing selected, Up and Down both land on the first item
    // rather than Up wrapping to the last -- the same thing Windows
    // does, and the alternative surprises anyone who just tabbed in.
    case KEY_ARROW_UP:   next = (sel < 0) ? 0 : sel - 1; break;
    case KEY_ARROW_DOWN: next = (sel < 0) ? 0 : sel + 1; break;
    case KEY_PAGE_UP:    next = (sel < 0) ? 0 : sel - rows; break;
    case KEY_PAGE_DOWN:  next = (sel < 0) ? 0 : sel + rows; break;
    case KEY_HOME:       next = 0; break;
    case KEY_END:        next = lb->count - 1; break;
    default:
        // Enter and Esc deliberately fall through -- see ui_listbox.h.
        return 0;
    }

    // Clamp rather than wrap. A list that jumps from the last item to
    // the first on one more Down keypress loses the user's place, and
    // neither Windows' listbox nor its combo popup wraps.
    if (next < 0) next = 0;
    if (next >= lb->count) next = lb->count - 1;

    lb->selected = next;
    scroll_into_view(lb, next);
    return 1;
}

// ---- focus integration ----------------------------------------------
//
// One const table, so this widget joins a ui_focus ring without
// ui_focus.c knowing it exists (see ui_focus.h). The wrappers exist
// only to adapt signatures -- there is deliberately no behaviour here
// that ui_listbox_key()/_hit() don't already have.
static int focus_key(void *w, int key, uint8_t mods) {
    (void)mods; // no listbox binding needs a modifier yet
    return ui_listbox_key((struct ui_listbox *)w, key);
}

static int focus_hit(const void *w, int cx, int cy) {
    return ui_listbox_hit((const struct ui_listbox *)w, cx, cy);
}

static void focus_ring(const void *w, int ox, int oy, uint32_t color) {
    const struct ui_listbox *lb = (const struct ui_listbox *)w;
    ui_focus_ring_rect(lb->x, lb->y, lb->w, lb->h, ox, oy, color);
}

static int focus_accepts(const void *w) {
    const struct ui_listbox *lb = (const struct ui_listbox *)w;
    return !lb->disabled && lb->count > 0;
}

const struct ui_focus_ops ui_listbox_focus_ops = {
    .key = focus_key,
    .hit = focus_hit,
    .draw_ring = focus_ring,
    .accepts_focus = focus_accepts,
    .set_focused = 0, // no focus-only visual of its own; the ring is enough
};
