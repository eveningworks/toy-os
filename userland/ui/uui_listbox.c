// listbox. Split out of uwidgets.c -- see ui/uui_listbox.h.
#include "ui/uui_listbox.h"
#include "ui/uui_widget.h"  // the ops table the focus ring takes
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// ---------------------------------------------------------------------
// listbox
// ---------------------------------------------------------------------

void uui_listbox_init(struct uui_listbox *lb, int x, int y, int w, int h,
                       const char *const *items, int count) {
    lb->x = x; lb->y = y; lb->w = w; lb->h = h;
    lb->items = items;
    lb->count = count;
    lb->selected = count > 0 ? 0 : -1;
    lb->hovered = -1;
    uui_seek_reset(&lb->seek);
    lb->top = 0;
    lb->row_h = 0; // derive from the font
    lb->bar_w = 8;
    lb->bg = ugfx_rgb(255, 255, 255);
    lb->fg = ugfx_rgb(20, 20, 20);
    lb->sel_bg = ugfx_rgb(205, 220, 240);
    lb->sel_fg = ugfx_rgb(20, 20, 20);
    lb->track_bg = UTHEME_BUTTON_BG;
    lb->thumb_bg = ugfx_rgb(150, 155, 165);
    lb->thumb_grab = -1;
}

void uui_listbox_set_items(struct uui_listbox *lb, const char *const *items, int count) {
    lb->items = items;
    lb->count = count;
    if (lb->selected >= count) lb->selected = count > 0 ? count - 1 : -1;
    if (lb->top > count) lb->top = 0;
    lb->hovered = -1;
    uui_seek_reset(&lb->seek);
}

// Left inset for a row's label. Hoisted out of the draw so
// uui_listbox_natural_size() reserves exactly what the draw uses --
// the same "one geometry, shared" rule the scrollbar follows.
#define UUI_LISTBOX_PAD_X 4

int uui_listbox_row_h(const struct uui_listbox *lb) {
    return lb->row_h > 0 ? lb->row_h : ugfx_char_h() + 4;
}

int uui_listbox_visible_rows(const struct uui_listbox *lb) {
    int rh = uui_listbox_row_h(lb);
    int n = rh > 0 ? lb->h / rh : 0;
    return n > 0 ? n : 1;
}

int uui_listbox_scrollbar_visible(const struct uui_listbox *lb) {
    return lb->count > uui_listbox_visible_rows(lb);
}

// Clamps `top` to a range that always fills the box when it can -- a
// list scrolled past its end leaves blank rows below real ones, which
// reads as missing content rather than as the end of the list.
static void listbox_clamp(struct uui_listbox *lb) {
    int vis = uui_listbox_visible_rows(lb);
    int max_top = lb->count > vis ? lb->count - vis : 0;
    if (lb->top > max_top) lb->top = max_top;
    if (lb->top < 0) lb->top = 0;
}

// A SHIFTED COPY rather than an offset threaded through the body: the
// draw reads lb->x/lb->y in seven places and a missed one would paint
// one element at the wrong origin, which is the bug that does not look
// like an origin bug.
void uui_listbox_draw_at(struct ugfx_surface *s, const struct uui_listbox *lb,
                          int ox, int oy) {
    if (!ox && !oy) { uui_listbox_draw(s, lb); return; }
    struct uui_listbox t = *lb;
    t.x -= ox;
    t.y -= oy;
    uui_listbox_draw(s, &t);
}

void uui_listbox_draw(struct ugfx_surface *s, const struct uui_listbox *lb) {
    int rh = uui_listbox_row_h(lb);
    int vis = uui_listbox_visible_rows(lb);
    int bar = uui_listbox_scrollbar_visible(lb) ? lb->bar_w : 0;
    int text_w = lb->w - bar;

    ugfx_fill_rect(s, lb->x, lb->y, lb->w, lb->h, lb->bg);

    for (int i = 0; i < vis; i++) {
        int idx = lb->top + i;
        if (idx >= lb->count) break;
        int ry = lb->y + i * rh;

        uint32_t rbg = lb->bg, rfg = lb->fg;
        if (idx == lb->selected) { rbg = lb->sel_bg; rfg = lb->sel_fg; }
        else if (idx == lb->hovered) { rbg = uui_state_bg(lb->bg, UUI_STATE_HOVER); }

        if (rbg != lb->bg) ugfx_fill_rect(s, lb->x, ry, text_w, rh, rbg);
        ugfx_draw_string_clipped(s, lb->x + UUI_LISTBOX_PAD_X, ry + (rh - ugfx_char_h()) / 2,
                                  text_w - 8, lb->items[idx], rfg, rbg);
    }

    if (bar) {
        uui_scrollbar_draw(s, lb->x + lb->w - bar, lb->y, bar, lb->h,
                            lb->count, vis, lb->count - vis - lb->top,
                            lb->track_bg, lb->thumb_bg, 0);
    }

    // THE SELECTED ROW IS NOT THE FOCUS INDICATOR: a selected row looks
    // identical whether or not the list is the thing answering the
    // arrows, so two lists side by side say nothing about which one is
    // listening. The ring goes on that row, or round the box when it is
    // scrolled off or nothing is selected -- an indicator that vanishes
    // is not one.
    if (lb->focused) {
        int view = lb->selected - lb->top;
        if (lb->selected >= 0 && view >= 0 && view < vis)
            uui_focus_ring(s, lb->x, lb->y + view * rh, text_w, rh);
        else
            uui_focus_ring(s, lb->x, lb->y, lb->w, lb->h);
    }
}

void uui_listbox_natural_size(const struct uui_listbox *lb, int *out_w, int *out_h) {
    if (out_w) {
        int widest = 0;
        for (int i = 0; i < lb->count; i++) {
            int tw = ugfx_text_width(lb->items[i]);
            if (tw > widest) widest = tw;
        }
        *out_w = widest + UUI_LISTBOX_PAD_X * 2 + lb->bar_w;
    }
    if (out_h) *out_h = lb->count * uui_listbox_row_h(lb);
}

int uui_listbox_hit(const struct uui_listbox *lb, int cx, int cy) {
    int bar = uui_listbox_scrollbar_visible(lb) ? lb->bar_w : 0;
    if (!uui_hit(lb->x, lb->y, lb->w - bar, lb->h, cx, cy)) return -1;
    int rh = uui_listbox_row_h(lb);
    int idx = lb->top + (cy - lb->y) / rh;
    if (idx < 0 || idx >= lb->count) return -1;
    return idx;
}

int uui_listbox_hover(struct uui_listbox *lb, int cx, int cy) {
    int idx = uui_listbox_hit(lb, cx, cy);
    if (idx == lb->hovered) return 0;
    lb->hovered = idx;
    return 1;
}

int uui_listbox_click(struct uui_listbox *lb, int cx, int cy) {
    int idx = uui_listbox_hit(lb, cx, cy);
    if (idx < 0 || idx == lb->selected) return 0;
    lb->selected = idx;
    return 1;
}

// --- the scrollbar's own input -----------------------------------------
//
// A listbox that DREW a scrollbar and handled none of its input shipped
// in ring 3 and was found by using the desktop, not by the suite -- the
// third time this project has shipped a bar that draws and does nothing
// (see apps/ui/ui_textview.h for the first two). The behaviour belongs
// to the widget: an app forwards its events and gets a real scrollbar,
// rather than each app growing its own copy of this and one of them
// getting it wrong.
//
// NOTE the offset inversion. `top` counts rows from the TOP, while the
// scrollbar's `scroll_offset` counts from the BOTTOM (0 = pinned to the
// newest, matching the text view). draw() already converts one way with
// `count - vis - top`; everything here has to convert back the same way,
// or the thumb tracks the cursor upside down.
static int listbox_bar_x(const struct uui_listbox *lb) {
    return lb->x + lb->w - lb->bar_w;
}

static int listbox_offset(const struct uui_listbox *lb) {
    return lb->count - uui_listbox_visible_rows(lb) - lb->top;
}

static int listbox_set_offset(struct uui_listbox *lb, int offset) {
    int before = lb->top;
    lb->top = lb->count - uui_listbox_visible_rows(lb) - offset;
    listbox_clamp(lb);
    return lb->top != before;
}

int uui_listbox_press(struct uui_listbox *lb, int cx, int cy) {
    if (!uui_listbox_scrollbar_visible(lb)) return 0;
    int bx = listbox_bar_x(lb);
    if (cx < bx || cx >= lb->x + lb->w) return 0;   // not on the strip
    if (cy < lb->y || cy >= lb->y + lb->h) return 0;

    int vis = uui_listbox_visible_rows(lb);
    int off = listbox_offset(lb);
    enum uui_scrollbar_zone zone =
        uui_scrollbar_hit(bx, lb->y, lb->bar_w, lb->h, lb->count, vis, off,
                          cx, cy, 0);

    if (zone == UUI_SB_THUMB) {
        int thumb_y, thumb_h;
        uui_scrollbar_thumb_rect(lb->y, lb->h, lb->count, vis, off,
                                  &thumb_y, &thumb_h, lb->bar_w, 0);
        // The grab offset WITHIN the thumb, so the thumb tracks the
        // cursor instead of snapping its top to it -- the exact bug the
        // ring-3 Notepad shipped by passing 0 here.
        lb->thumb_grab = cy - thumb_y;
        return 1;
    }

    // Track: page toward the click, keeping one row of overlap, which is
    // what every real toolkit does.
    int page = vis > 1 ? vis - 1 : 1;
    if (zone == UUI_SB_ABOVE) listbox_set_offset(lb, off + page);
    else if (zone == UUI_SB_BELOW) listbox_set_offset(lb, off - page);
    else return 0;
    return 1;
}

int uui_listbox_drag(struct uui_listbox *lb, int cx, int cy) {
    (void)cx;
    if (lb->thumb_grab < 0) return 0;
    int vis = uui_listbox_visible_rows(lb);
    int off = uui_scrollbar_offset_for_drag(lb->y, lb->h, lb->count, vis,
                                             cy, lb->thumb_grab, lb->bar_w, 0);
    return listbox_set_offset(lb, off);
}

void uui_listbox_drag_end(struct uui_listbox *lb) {
    lb->thumb_grab = -1;
}

int uui_listbox_wheel(struct uui_listbox *lb, int notches) {
    int before = lb->top;
    // Scrolling must NOT change the selection -- a wheel over a list is
    // navigation, not a choice. The kernel version documents the same.
    lb->top -= notches * 3;
    listbox_clamp(lb);
    return lb->top != before;
}

// Keeps the selected row inside the visible window after a keyboard
// move, which is the whole reason arrow keys feel broken without it.
static void listbox_reveal(struct uui_listbox *lb) {
    int vis = uui_listbox_visible_rows(lb);
    if (lb->selected < lb->top) lb->top = lb->selected;
    else if (lb->selected >= lb->top + vis) lb->top = lb->selected - vis + 1;
    listbox_clamp(lb);
}

// --- type-ahead --------------------------------------------------------
//
// The search itself is ui/uui_seek.h's, shared with uui_table. All this
// supplies is the text of one item.

static void lb_seek_text(void *ctx, int idx, char *out, int cap) {
    const struct uui_listbox *lb = (const struct uui_listbox *)ctx;
    const char *it = (idx >= 0 && idx < lb->count) ? lb->items[idx] : 0;
    int i = 0;
    if (it) for (; i < cap - 1 && it[i]; i++) out[i] = it[i];
    out[i] = '\0';
}

int uui_listbox_key(struct uui_listbox *lb, int key) {
    if (lb->count <= 0) return 0;
    int before = lb->selected;

    if (key == KEY_ARROW_UP)        { if (lb->selected > 0) lb->selected--; }
    else if (key == KEY_ARROW_DOWN) { if (lb->selected < lb->count - 1) lb->selected++; }
    else if (key == KEY_HOME)       { lb->selected = 0; }
    else if (key == KEY_END)        { lb->selected = lb->count - 1; }
    else if (key == KEY_PAGE_UP)    { lb->selected -= uui_listbox_visible_rows(lb);
                                       if (lb->selected < 0) lb->selected = 0; }
    else if (key == KEY_PAGE_DOWN)  { lb->selected += uui_listbox_visible_rows(lb);
                                       if (lb->selected >= lb->count) lb->selected = lb->count - 1; }
    // A PRINTABLE KEY IS A SEARCH, not a keystroke to pass on.
    else if (uui_seek_is_key(key)) {
        int idx = uui_seek_key(&lb->seek, key, lb->count, lb->selected,
                                lb_seek_text, lb);
        if (idx < 0) return 0;
        lb->selected = idx;
        listbox_reveal(lb);
        return 1;
    }
    else return 0;

    // Any of the movement keys above ends a search in progress -- the
    // next letter should start a new one, not extend a prefix the user
    // has stopped thinking about.
    uui_seek_reset(&lb->seek);

    listbox_reveal(lb);
    return lb->selected != before;
}

// --- focus ------------------------------------------------------------
//
// Focus-only ops (see uui_textbox.c's note on why these tables are not
// full ones). It carries set_focused -- see the ring in the draw above.
//
// `>= 0`, NOT the row index.
//
// uui_listbox_hit() returns a ROW, and the router tests this slot as a
// BOOLEAN (`!it->ops->hit(...)` in ui/uui_route.c) -- so row 0, the one
// row whose index is falsey, reported "not hit" and could not be
// selected by clicking, while every other row worked. A first row that
// ignores clicks is easy to miss precisely because the widget is
// otherwise fine: it draws, it scrolls, it highlights on hover.
//
// Found by building uui_table, which reproduced the bug by copying this
// line. uui_radio_list had it right all along; the other widgets' _hit
// functions return uui_hit(), which is already a boolean.
static int lb_ops_hit(const void *w, int cx, int cy) {
    return uui_listbox_hit((const struct uui_listbox *)w, cx, cy) >= 0;
}
static int lb_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_listbox_key((struct uui_listbox *)w, key);
}
static int lb_ops_accepts_focus(const void *w) { (void)w; return 1; }
static void lb_ops_set_focused(void *w, int focused) {
    ((struct uui_listbox *)w)->focused = focused;
}

const struct uui_widget_ops uui_listbox_focus_ops = {
    .hit = lb_ops_hit,
    .key = lb_ops_key,
    .accepts_focus = lb_ops_accepts_focus,
    .set_focused   = lb_ops_set_focused,
};

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// With these filled in, an app declares a listbox and never mentions
// input again: the router hit-tests it, a press starts a thumb drag or
// selects a row, and the pointer GRAB keeps that drag alive when the
// cursor leaves the control. This is what the app-side forwarding this
// widget used to require became.
static int lb_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_listbox *lb = (struct uui_listbox *)w;
    // The scrollbar outranks the rows: uui_listbox_hit() excludes the
    // bar column, so a press there has to be offered to the bar first
    // or it reaches nothing at all -- which is exactly how this
    // widget's scrollbar shipped dead.
    if (uui_listbox_press(lb, cx, cy)) return 1;
    return uui_listbox_click(lb, cx, cy);
}

static int lb_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_listbox *lb = (struct uui_listbox *)w;
    if (buttons) return uui_listbox_drag(lb, cx, cy);
    return uui_listbox_hover(lb, cx, cy);
}

static int lb_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    uui_listbox_drag_end((struct uui_listbox *)w);
    return 0;
}

static int lb_ops_wheel(void *w, int notches) {
    return uui_listbox_wheel((struct uui_listbox *)w, notches);
}

// A LAYOUT-PARTICIPATING widget needs both of these. Without them the
// layout has no way to place a listbox at all, so one declared in
// uapp_desc.widgets drew at whatever coordinates init() was given --
// which for a widget the app expected to be positioned FOR it means on
// top of its siblings, outside the content area, looking like a
// clipping bug rather than a missing ops slot.
static void lb_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_listbox_natural_size((const struct uui_listbox *)w, out_w, out_h);
}

static void lb_ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_listbox *lb = (struct uui_listbox *)w;
    lb->x = x;
    lb->y = y;
    lb->w = width;
    lb->h = height;
    // The visible-row count and the scrollbar both derive from h, and
    // both are recomputed on demand from it (see uui_listbox_visible_
    // rows()), so there is nothing cached here to invalidate.
}

static int lb_ops_bounds(const void *w, int cx, int cy) {
    const struct uui_listbox *lb = (const struct uui_listbox *)w;
    // The WHOLE control, scrollbar strip included -- unlike
    // uui_listbox_hit(), which answers "which ROW" and deliberately
    // excludes the bar. Two different questions, and conflating them is
    // how a press on the bar reached nothing.
    return uui_hit(lb->x, lb->y, lb->w, lb->h, cx, cy);
}

static void lb_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_listbox_draw(s, (const struct uui_listbox *)w);
}

static void lb_ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_listbox *lb = (const struct uui_listbox *)w;
    uui_describe_int(d, "row_h", uui_listbox_row_h(lb));
    uui_describe_int(d, "selected", lb->selected);
}

static void lb_rect_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_listbox *s = (const struct uui_listbox *)w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

const struct uui_widget_ops uui_listbox_ops = {
    .natural_size  = lb_ops_natural_size,
    .set_geometry  = lb_ops_set_geometry,
    .draw          = lb_ops_draw,
    .hit           = lb_ops_bounds,
    .key           = lb_ops_key,
    .accepts_focus = lb_ops_accepts_focus,
    .set_focused   = lb_ops_set_focused,
    .press         = lb_ops_press,
    .motion        = lb_ops_motion,
    .release       = lb_ops_release,
    .wheel         = lb_ops_wheel,
    .describe      = lb_ops_describe,
    .bounds = lb_rect_op,
};
