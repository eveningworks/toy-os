// See ui_dropdown.h for the design writeup -- in particular why the
// popup is a separate draw call, and why it composes ui_listbox rather
// than carrying a list of its own.
#include "ui_dropdown.h"
#include "ui_focus.h"
#include "kapi.h"

#define DEFAULT_MAX_ROWS 8
// Gap between the closed box and its popup. One pixel, so the two read
// as connected rather than as two unrelated panels.
#define POPUP_GAP 1

void ui_dropdown_init(struct ui_dropdown *dd, int x, int y, int w, int h,
                       const char *const *items, int count, int selected,
                       uint32_t bg, uint32_t fg, uint32_t border,
                       uint32_t sel_bg, uint32_t sel_fg,
                       uint32_t track_bg, uint32_t thumb_bg) {
    dd->x = x; dd->y = y; dd->w = w; dd->h = h;
    dd->open = 0;
    dd->max_rows = DEFAULT_MAX_ROWS;
    dd->disabled = 0;
    dd->hovered = 0;
    dd->pressed = 0;
    dd->value_on_open = selected;
    dd->bg = bg;
    dd->fg = fg;
    dd->border = border;
    dd->popup_x = dd->popup_y = dd->popup_w = dd->popup_h = 0;

    // The popup's list uses the BOX's background, not the window's, so
    // the two read as one control. Geometry is a placeholder: layout()
    // recomputes it against the box on every draw and every hit test.
    ui_listbox_init(&dd->list, x, y, w, h, items, count,
                     bg, fg, sel_bg, sel_fg, track_bg, thumb_bg);
    ui_listbox_set_selected(&dd->list, selected);
}

void ui_dropdown_set_geometry(struct ui_dropdown *dd, int x, int y, int w, int h) {
    dd->x = x; dd->y = y; dd->w = w; dd->h = h;
}

int ui_dropdown_selected(const struct ui_dropdown *dd) { return dd->list.selected; }

const char *ui_dropdown_selected_text(const struct ui_dropdown *dd) {
    int s = dd->list.selected;
    if (s < 0 || s >= dd->list.count || !dd->list.items) return 0;
    return dd->list.items[s];
}

void ui_dropdown_set_selected(struct ui_dropdown *dd, int index) {
    ui_listbox_set_selected(&dd->list, index);
}

// Resolves where the popup goes, and commits it to both the cached
// popup_* fields and the embedded listbox's geometry -- so drawing and
// hit-testing read the SAME numbers rather than each deriving them.
//
// The popup wants to hang below the box. It cannot leave the window (the
// WM clips on_draw to the content rect), and this control has no way to
// ask how tall the content area is, so `avail_h` is what the caller
// knows: the draw origin tells us where content y=0 is, and everything
// below the box down to the window's bottom edge is what we may use.
// Rather than plumb a window height in, the rule is simpler and needs no
// new information: prefer below, flip above when the space above is
// bigger, and shrink to whichever side was chosen.
static void layout(struct ui_dropdown *dd, int avail_below, int avail_above) {
    int rows = dd->list.count;
    if (rows > dd->max_rows) rows = dd->max_rows;
    if (rows < 1) rows = 1;

    int want_h = ui_listbox_height_for_rows(&dd->list, rows);
    int below_h = avail_below - POPUP_GAP;
    int above_h = avail_above - POPUP_GAP;
    if (below_h < 0) below_h = 0;
    if (above_h < 0) above_h = 0;

    int flip = 0;
    if (want_h > below_h && above_h > below_h) flip = 1;

    int room = flip ? above_h : below_h;
    int h = want_h < room ? want_h : room;
    // Always show at least one row: a zero-height popup is invisible and
    // unclickable, and reads as the dropdown being broken rather than as
    // the window being too small.
    int min_h = ui_listbox_height_for_rows(&dd->list, 1);
    if (h < min_h) h = min_h;

    dd->popup_w = dd->w;
    dd->popup_x = dd->x;
    dd->popup_y = flip ? (dd->y - POPUP_GAP - h) : (dd->y + dd->h + POPUP_GAP);
    dd->popup_h = h;

    ui_listbox_set_geometry(&dd->list, dd->popup_x, dd->popup_y, dd->popup_w, dd->popup_h);
}

// The space the popup may use, derived from the draw origin and the
// screen. `origin_y + dd->y` is the box's absolute position, and the
// window's own clip stops anything past its content rect -- so bounding
// against the screen is the honest upper limit this control can compute
// on its own. A window whose content ends well above the screen bottom
// gets a popup clipped by the WM rather than a wrongly-placed one; the
// flip rule below still fires on the common case (a box near the bottom
// of a tall window near the bottom of the screen).
static void layout_for_origin(struct ui_dropdown *dd, int origin_y) {
    int abs_y = origin_y + dd->y;
    int below = gfx_height() - (abs_y + dd->h);
    int above = abs_y;
    layout(dd, below > 0 ? below : 0, above > 0 ? above : 0);
}

void ui_dropdown_open(struct ui_dropdown *dd) {
    if (dd->disabled) return;
    dd->open = 1;
    dd->value_on_open = dd->list.selected;
    // Scroll the current value into view, so the list opens showing what
    // it is set to rather than at the top.
    ui_listbox_set_selected(&dd->list, dd->list.selected);
    dd->list.hovered = -1;
}

void ui_dropdown_close(struct ui_dropdown *dd) {
    dd->open = 0;
    dd->list.hovered = -1;
    dd->list.armed = -1;
    dd->list.pressing = 0;
    dd->list.bar_press = 0;
    dd->list.thumb_grab = -1;
}

int ui_dropdown_hit(const struct ui_dropdown *dd, int cx, int cy) {
    return widget_hit(dd->x, dd->y, dd->w, dd->h, cx, cy);
}

int ui_dropdown_popup_hit(const struct ui_dropdown *dd, int cx, int cy) {
    if (!dd->open) return 0;
    return widget_hit(dd->popup_x, dd->popup_y, dd->popup_w, dd->popup_h, cx, cy);
}

// The little downward triangle at the right end of the closed box. Drawn
// from the font metrics like everything else here, so it stays in
// proportion when `fontsize` changes.
static void draw_arrow(int ax, int ay, int h, uint32_t fg) {
    int size = gfx_char_w();
    if (size < 3) size = 3;
    int cx = ax - size;          // ax is the right inset edge
    int cy = ay + (h - size / 2) / 2;
    for (int i = 0; i < size / 2 + 1; i++) {
        int w = size - i * 2;
        if (w <= 0) break;
        gfx_fill_rect(cx + i, cy + i, w, 1, fg);
    }
}

void ui_dropdown_draw(struct ui_dropdown *dd, int origin_x, int origin_y) {
    // Lay out now, even though the popup is drawn later: hit-testing runs
    // from event handlers that never get a chance to compute geometry,
    // so the popup rect has to be current as of the last frame.
    layout_for_origin(dd, origin_y);

    int ax = origin_x + dd->x, ay = origin_y + dd->y;

    enum ui_state state = UI_STATE_REST;
    if (dd->disabled) state = UI_STATE_DISABLED;
    else if (dd->pressed || dd->open) state = UI_STATE_PRESSED;
    else if (dd->hovered) state = UI_STATE_HOVER;

    uint32_t bg = ui_state_bg(dd->bg, state);
    uint32_t fg = dd->disabled ? ui_state_bg(dd->fg, UI_STATE_DISABLED) : dd->fg;

    gfx_fill_rect(ax, ay, dd->w, dd->h, bg);
    gfx_draw_rect(ax, ay, dd->w, dd->h, dd->border);

    int pad = gfx_char_w() / 2;
    int arrow_room = gfx_char_w() * 2;
    const char *text = ui_dropdown_selected_text(dd);
    // Clipped, and budgeted against the arrow rather than the box edge --
    // gfx_draw_string() would happily draw the label straight through it
    // (docs/gui-guidelines.md).
    gfx_draw_string_clipped(ax + pad, ay + (dd->h - gfx_char_h()) / 2,
                             dd->w - pad * 2 - arrow_room,
                             text ? text : "", fg, bg);
    draw_arrow(ax + dd->w - pad, ay, dd->h, fg);
}

void ui_dropdown_draw_popup(struct ui_dropdown *dd, int origin_x, int origin_y) {
    if (!dd->open) return; // always safe to call -- see ui_dropdown.h
    layout_for_origin(dd, origin_y);
    ui_listbox_draw(&dd->list, origin_x, origin_y);
    gfx_draw_rect(origin_x + dd->popup_x, origin_y + dd->popup_y,
                   dd->popup_w, dd->popup_h, dd->border);
}

int ui_dropdown_hover(struct ui_dropdown *dd, int cx, int cy) {
    if (dd->disabled) return 0;
    int changed = 0;

    if (dd->open) {
        // The popup is on top, so it owns the hover wherever it covers.
        if (ui_listbox_hover(&dd->list, cx, cy)) changed = 1;
        if (ui_dropdown_popup_hit(dd, cx, cy)) {
            if (dd->hovered) { dd->hovered = 0; changed = 1; }
            return changed;
        }
    }

    int now = ui_dropdown_hit(dd, cx, cy) ? 1 : 0;
    if (now != dd->hovered) { dd->hovered = now; changed = 1; }
    return changed;
}

int ui_dropdown_press(struct ui_dropdown *dd, int cx, int cy) {
    if (dd->disabled) return 0;

    if (dd->open) {
        // A press already being tracked by the list (a thumb drag, a
        // row) stays with it even once the cursor leaves the popup.
        if (dd->list.pressing || dd->list.thumb_grab >= 0) {
            ui_listbox_press(&dd->list, cx, cy);
            return 1;
        }
        if (ui_dropdown_popup_hit(dd, cx, cy)) {
            ui_listbox_press(&dd->list, cx, cy);
            return 1;
        }
        // Outside the popup. A press on the BOX itself closes it (the
        // second click of an open/close pair); anywhere else dismisses
        // it and is deliberately NOT consumed, so the click still
        // reaches whatever it landed on -- which is what makes a
        // dropdown feel like it isn't in the way.
        if (ui_dropdown_hit(dd, cx, cy)) {
            dd->pressed = 1;
            return 1;
        }
        ui_dropdown_close(dd);
        return 0;
    }

    if (ui_dropdown_hit(dd, cx, cy)) {
        dd->pressed = 1;
        dd->hovered = 0; // the press visual owns the feedback now
        return 1;
    }
    dd->pressed = 0;
    return 0;
}

int ui_dropdown_release(struct ui_dropdown *dd) {
    int was_pressed = dd->pressed;
    dd->pressed = 0;
    if (dd->disabled) return -1;

    if (dd->open) {
        // A press that started on the box while open: this is the
        // close half of a click-to-open/click-to-close pair.
        if (was_pressed) {
            ui_dropdown_close(dd);
            return -1;
        }
        int before = dd->value_on_open;
        int picked = ui_listbox_release(&dd->list);
        if (picked < 0) return -1; // a scrollbar gesture, or dragged off a row
        ui_dropdown_close(dd);
        // Only report a CHANGE. Re-picking the value it already had is
        // a legitimate way to dismiss the list, and an app acting on it
        // would re-run whatever the value drives for no reason.
        return picked != before ? picked : -1;
    }

    if (was_pressed) ui_dropdown_open(dd);
    return -1; // opening is not a value change
}

int ui_dropdown_wheel(struct ui_dropdown *dd, int delta) {
    if (dd->disabled) return 0;
    // Ignored while closed, on purpose -- see the header. Scrolling past
    // a combo must not silently change what it is set to.
    if (!dd->open) return 0;
    return ui_listbox_wheel(&dd->list, delta);
}

int ui_dropdown_key(struct ui_dropdown *dd, int key) {
    if (dd->disabled) return 0;

    if (dd->open) {
        if (key == '\n' || key == '\r') {
            ui_dropdown_close(dd);
            return 1;
        }
        if (key == 0x1b) { // Esc -- restore the value the popup opened with
            ui_listbox_set_selected(&dd->list, dd->value_on_open);
            ui_dropdown_close(dd);
            return 1;
        }
        return ui_listbox_key(&dd->list, key);
    }

    // Closed: arrows change the value directly, which is what lets a
    // combo be operated without ever opening it. Enter/Esc are left
    // alone so a dialog's default and cancel buttons still see them.
    if (key == KEY_ARROW_UP || key == KEY_ARROW_DOWN ||
        key == KEY_HOME || key == KEY_END ||
        key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        return ui_listbox_key(&dd->list, key);
    }
    return 0;
}

// ---- focus integration ----------------------------------------------
//
// See ui_focus.h. `hit` covers the OPEN POPUP as well as the closed box,
// so clicking a popup row keeps focus on the dropdown that owns it
// rather than moving focus to whatever widget the popup happens to be
// covering -- the popup is drawn over other widgets by design, and
// focus has to follow what the user sees, not what is underneath.
static int focus_key(void *w, int key, uint8_t mods) {
    (void)mods;
    return ui_dropdown_key((struct ui_dropdown *)w, key);
}

static int focus_hit(const void *w, int cx, int cy) {
    const struct ui_dropdown *dd = (const struct ui_dropdown *)w;
    return ui_dropdown_hit(dd, cx, cy) || ui_dropdown_popup_hit(dd, cx, cy);
}

static void focus_ring(const void *w, int ox, int oy, uint32_t color) {
    const struct ui_dropdown *dd = (const struct ui_dropdown *)w;
    // Always the CLOSED box, even while the popup is open: the ring says
    // where the keyboard is, and that is this control either way.
    ui_focus_ring_rect(dd->x, dd->y, dd->w, dd->h, ox, oy, color);
}

static int focus_accepts(const void *w) {
    return !((const struct ui_dropdown *)w)->disabled;
}

// Losing focus closes an open popup. A list left hanging over other
// widgets after the keyboard has moved elsewhere is a stale overlay, and
// it would still be swallowing clicks aimed at what it covers.
static void focus_set(void *w, int focused) {
    if (!focused) ui_dropdown_close((struct ui_dropdown *)w);
}

const struct ui_focus_ops ui_dropdown_focus_ops = {
    .key = focus_key,
    .hit = focus_hit,
    .draw_ring = focus_ring,
    .accepts_focus = focus_accepts,
    .set_focused = focus_set,
};
