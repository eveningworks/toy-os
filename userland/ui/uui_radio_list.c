// radio list. Split out of uwidgets.c -- see ui/uui_radio_list.h.
#include "ui/uui_radio_list.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include "ui/uui_widget.h"  // the ops table at the bottom of this file

// ---------------------------------------------------------------------
// radio list
// ---------------------------------------------------------------------

static int radio_rows(const struct uui_radio_list *l) {
    int cols = l->cols > 0 ? l->cols : 1;
    return (l->count + cols - 1) / cols;
}

// FONT-DERIVED DEFAULTS for the three metrics, used whenever the caller
// left one at 0. This widget has no init() -- it is set up by assigning
// its fields -- so every metric a caller forgot used to stay 0, and a
// row_h or col_w of 0 makes the whole control zero-sized: it draws
// nothing, hit-tests nothing, and reports a natural size of nothing, so
// a layout dutifully gives it no room. Invisible AND unclickable, with
// no error anywhere, from one unassigned field.
//
// Deriving them instead is the same treatment uui_listbox_row_h()
// already gives its own row height, and it keeps layout font-derived
// per docs/gui-guidelines.md. An explicit non-zero value still wins.
static int radio_marker(const struct uui_radio_list *l) {
    if (l->marker_size > 0) return l->marker_size;
    int m = ugfx_char_h() - 2;
    return m > 6 ? m : 6;
}

static int radio_row_h(const struct uui_radio_list *l) {
    return l->row_h > 0 ? l->row_h : ugfx_char_h() + 6;
}

static int radio_col_w(const struct uui_radio_list *l) {
    if (l->col_w > 0) return l->col_w;
    // The WIDEST option, not the selected one -- a column sized to
    // today's value and clipping tomorrow's is the bug
    // uui_dropdown_natural_size() documents avoiding, and it applies
    // just as hard here.
    int widest = 0;
    for (int i = 0; i < l->count; i++) {
        int tw = ugfx_text_width(l->options[i]);
        if (tw > widest) widest = tw;
    }
    return widest + radio_marker(l) + ugfx_char_w() * 2;
}

void uui_radio_list_natural_size(const struct uui_radio_list *l, int *out_w, int *out_h) {
    int cols = l->cols > 0 ? l->cols : 1;
    if (out_w) *out_w = cols * radio_col_w(l);
    if (out_h) *out_h = radio_rows(l) * radio_row_h(l);
}

static void radio_cell(const struct uui_radio_list *l, int i, int x, int y, int *cx, int *cy) {
    int cols = l->cols > 0 ? l->cols : 1;
    *cx = x + (i % cols) * radio_col_w(l);
    *cy = y + (i / cols) * radio_row_h(l);
}

void uui_radio_list_set_geometry(struct uui_radio_list *l, int x, int y) {
    l->x = x;
    l->y = y;
    uui_radio_list_natural_size(l, &l->w, &l->h);
}

void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l) {
    int selected = l->selected, hovered = l->hovered;
    uint32_t bg = l->bg, fg = l->fg;
    // Dimmed, and with no hover or focus ring: those say "this responds
    // to you", which is the one thing a disabled control must not say.
    if (l->disabled) {
        fg = uui_state_bg(fg, UUI_STATE_DISABLED);
        hovered = -1;
    }
    int x = l->x, y = l->y;
    for (int i = 0; i < l->count; i++) {
        int cx, cy;
        radio_cell(l, i, x, y, &cx, &cy);

        uint32_t row_bg = bg;
        if (i == hovered) {
            row_bg = uui_state_bg(bg, UUI_STATE_HOVER);
            ugfx_fill_rect(s, cx, cy, radio_col_w(l), radio_row_h(l), row_bg);
        }

        int m = radio_marker(l);
        int my = cy + (radio_row_h(l) - m) / 2;
        ugfx_draw_rect(s, cx, my, m, m, fg);
        if (i == selected) {
            int inset = m / 4 > 0 ? m / 4 : 1;
            ugfx_fill_rect(s, cx + inset, my + inset, m - 2 * inset, m - 2 * inset, fg);
        }
        ugfx_draw_string_clipped(s, cx + m + 6, cy + (radio_row_h(l) - ugfx_char_h()) / 2,
                                  radio_col_w(l) - m - 8, l->options[i], fg, row_bg);

        // The focus indicator rings the SELECTED row, not a separate
        // cursor row, because arrowing moves the selection itself here
        // (see uui_radio_list_key) -- a second highlight would be a
        // position that can never differ from this one.
        if (l->focused && i == selected) {
            ugfx_draw_rect(s, cx, cy, radio_col_w(l) - 2, radio_row_h(l) - 2,
                            uui_state_bg(fg, UUI_STATE_HOVER));
        }
    }
}

int uui_radio_list_hit(const struct uui_radio_list *l, int cx, int cy) {
    int x = l->x, y = l->y, px = cx, py = cy;
    for (int i = 0; i < l->count; i++) {
        int cx, cy;
        radio_cell(l, i, x, y, &cx, &cy);
        if (uui_hit(cx, cy, radio_col_w(l), radio_row_h(l), px, py)) return i;
    }
    return -1;
}

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// The widget owns `selected` now. It used to be the APP's -- draw()
// takes the value as an argument -- which meant a routed press had
// nowhere to record a choice, and two apps could disagree about which
// option was current. draw() still accepts an argument (callers pass
// `l->selected`), so nothing that already worked changed.
static int rl_ops_hit(const void *w, int cx, int cy) {
    return uui_radio_list_hit((const struct uui_radio_list *)w, cx, cy) >= 0;
}

// ARM on press: move the selection so the user sees what they are
// choosing, but remember what it was. Nothing is committed here -- the
// app is not told until release, which is what lets a press dragged off
// the list change nothing.
static int rl_ops_press(void *w, int cx, int cy) {
    struct uui_radio_list *l = (struct uui_radio_list *)w;
    if (l->disabled) return 0;
    int idx = uui_radio_list_hit(l, cx, cy);
    if (idx < 0) return 0;
    l->armed_prev = l->selected;
    l->selected = idx;
    // Non-zero on ANY hit, including a press on the row that is already
    // selected. The router only takes a pointer grab when press returns
    // non-zero, and without the grab no release is delivered -- so
    // returning 0 here would silently break the commit-on-release cycle
    // for exactly one row.
    return 1;
}

// COMMIT on release, or cancel if the pointer left the list.
//
// Returning non-zero is also what makes the ROUTER report this widget's
// id to the app at all (uui_route.c only names a widget that has a
// release op), so without this pair an app acting on release never
// heard a radio list change -- which is exactly what happened the first
// time Control Panel was told to stop acting on motion.
static int rl_ops_release(void *w, int cx, int cy) {
    struct uui_radio_list *l = (struct uui_radio_list *)w;
    if (l->disabled) return 0;
    int idx = uui_radio_list_hit(l, cx, cy);
    if (idx < 0 && l->armed_prev >= 0) {
        l->selected = l->armed_prev; // dragged off -- put it back
    }
    l->armed_prev = -1;
    return 1; // always: the app decides, and the visual may have moved
}

static int rl_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_radio_list *l = (struct uui_radio_list *)w;
    if (l->disabled) return 0;
    int hot = uui_radio_list_hit(l, cx, cy);
    if (hot == l->hovered) return 0;
    l->hovered = hot;
    return 1;
}

// See the note on uui_listbox's pair: without these a radio list
// declared in a layout drew wherever init() left it.
//
// The height/width handed in are IGNORED on purpose -- a radio list
// cannot be stretched into a size its rows and columns do not produce,
// which uui_radio_list_set_geometry() has always said by taking only
// x and y. The ops slot's own comment (uui_widget.h) allows exactly
// this, and it is why natural_size has to be published alongside: a
// container that cannot ask how big this wants to be would hand it a
// cell it then declines to fill.
static void rl_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_radio_list_natural_size((const struct uui_radio_list *)w, out_w, out_h);
}

static void rl_ops_set_geometry(void *w, int x, int y, int width, int height) {
    (void)width; (void)height;
    uui_radio_list_set_geometry((struct uui_radio_list *)w, x, y);
}

static void rl_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_radio_list_draw(s, (const struct uui_radio_list *)w);
}

int uui_radio_list_key(struct uui_radio_list *l, int key) {
    if (l->count <= 0) return 0;
    int cols = l->cols > 0 ? l->cols : 1;
    int step;
    switch (key) {
        // In a multi-column grid, left/right step by one and up/down
        // step by a ROW -- the movement the layout implies, not the
        // array order, which is what makes a 2-column list feel like a
        // grid rather than a list folded in half.
        case KEY_ARROW_LEFT:  step = -1;    break;
        case KEY_ARROW_RIGHT: step = 1;     break;
        case KEY_ARROW_UP:    step = -cols; break;
        case KEY_ARROW_DOWN:  step = cols;  break;
        default: return 0;
    }
    int next = (l->selected < 0 ? 0 : l->selected + step);
    if (next < 0 || next >= l->count) return 0; // at an end -- consumed nothing, moved nothing
    if (next == l->selected) return 0;
    l->selected = next;
    return 1;
}

static int rl_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_radio_list *l = (struct uui_radio_list *)w;
    if (l->disabled) return 0;
    return uui_radio_list_key(l, key);
}

static void rl_ops_set_focused(void *w, int focused) {
    ((struct uui_radio_list *)w)->focused = focused;
}

// A list with no options has no option to choose, so it is skipped in
// the tab order rather than being a stop that does nothing -- the same
// call uui_slider_ops makes.
static int rl_ops_accepts_focus(const void *w) {
    const struct uui_radio_list *l = (const struct uui_radio_list *)w;
    return !l->disabled && l->count > 0;
}

const struct uui_widget_ops uui_radio_list_ops = {
    .natural_size = rl_ops_natural_size,
    .set_geometry = rl_ops_set_geometry,
    .draw   = rl_ops_draw,
    .hit    = rl_ops_hit,
    .press  = rl_ops_press,
    .release = rl_ops_release,
    .motion = rl_ops_motion,
    .key    = rl_ops_key,
    .set_focused   = rl_ops_set_focused,
    .accepts_focus = rl_ops_accepts_focus,
};
