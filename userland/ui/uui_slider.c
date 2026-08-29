// slider -- see ui/uui_slider.h for why the stops are discrete.
#include "ui/uui_slider.h"
#include "ui/uui_widget.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include <stddef.h>

#define SLIDER_PAD_X   6  // inset so the end thumbs are not half off
#define SLIDER_TRACK_H 4
#define SLIDER_THUMB_W 10
#define SLIDER_TICK_H  4
#define SLIDER_LABEL_GAP 4

void uui_slider_init(struct uui_slider *s, const char *const *options, int count) {
    s->x = s->y = s->w = s->h = 0;
    s->options = options;
    s->count = count;
    s->selected = count > 0 ? 0 : -1;
    s->hovered = 0;
    s->dragging = 0;
    s->bg = ugfx_rgb(245, 245, 245);
    s->fg = ugfx_rgb(20, 20, 20);
    s->track_bg = ugfx_rgb(205, 205, 212);
    // The travelled part of the track, so "how far along am I" reads
    // without counting ticks -- what every real slider draws.
    s->fill_bg = ugfx_rgb(120, 150, 200);
    s->thumb_bg = ugfx_rgb(70, 100, 160);
    s->disabled = 0;
}

void uui_slider_set_options(struct uui_slider *s, const char *const *options, int count) {
    s->options = options;
    s->count = count;
    if (s->selected >= count) s->selected = count > 0 ? count - 1 : -1;
    s->dragging = 0;
}

// --- geometry ---------------------------------------------------------

static int track_y(const struct uui_slider *s) {
    return s->y + SLIDER_THUMB_W / 2;
}

static int track_x0(const struct uui_slider *s) { return s->x + SLIDER_PAD_X; }
static int track_x1(const struct uui_slider *s) { return s->x + s->w - SLIDER_PAD_X; }

// The x of stop `i`. With one stop the whole track collapses to its
// start rather than dividing by zero -- a one-option enum is legal (a
// keyboard layout directory with one file) and must not crash.
static int stop_x(const struct uui_slider *s, int i) {
    int x0 = track_x0(s), x1 = track_x1(s);
    if (s->count <= 1) return x0;
    if (i < 0) i = 0;
    if (i >= s->count) i = s->count - 1;
    return x0 + (x1 - x0) * i / (s->count - 1);
}

void uui_slider_natural_size(const struct uui_slider *s, int *out_w, int *out_h) {
    // Room for every label, not the current one: a control that resized
    // as its value changed would make the page reflow under the user's
    // own drag. Same rule as uui_tree's natural size ignoring what is
    // collapsed.
    int widest = 0;
    for (int i = 0; i < s->count; i++) {
        int lw = ugfx_text_width(s->options[i]);
        if (lw > widest) widest = lw;
    }
    // Wide enough that the stops are far enough apart to hit: the label
    // width is a floor, and so is one thumb per stop.
    int by_stops = s->count * (SLIDER_THUMB_W + 8);
    int w = widest + 2 * SLIDER_PAD_X;
    if (by_stops > w) w = by_stops;
    if (out_w) *out_w = w;
    if (out_h) *out_h = SLIDER_THUMB_W + SLIDER_LABEL_GAP + ugfx_char_h();
}

void uui_slider_set_geometry(struct uui_slider *s, int x, int y, int w, int h) {
    s->x = x; s->y = y;
    s->w = w;
    // The height is the widget's own: it is a track, a thumb and one
    // line of text, and stretching that vertically would leave a gap
    // rather than a bigger control.
    int nh;
    uui_slider_natural_size(s, NULL, &nh);
    s->h = h > nh ? nh : (h > 0 ? h : nh);
}

// --- drawing ----------------------------------------------------------

void uui_slider_draw(struct ugfx_surface *surf, const struct uui_slider *s) {
    if (s->count <= 0) return;
    int ty = track_y(s), x0 = track_x0(s), x1 = track_x1(s);
    int sel = s->selected < 0 ? 0 : s->selected;
    int sx = stop_x(s, sel);

    ugfx_fill_rect(surf, x0, ty - SLIDER_TRACK_H / 2, x1 - x0, SLIDER_TRACK_H,
                    s->track_bg);
    // The travelled part.
    if (sx > x0)
        ugfx_fill_rect(surf, x0, ty - SLIDER_TRACK_H / 2, sx - x0, SLIDER_TRACK_H,
                        s->fill_bg);

    // A tick per stop, so the discreteness is VISIBLE -- a slider that
    // looked continuous and snapped would read as a bug rather than as
    // a design.
    for (int i = 0; i < s->count; i++) {
        int tx = stop_x(s, i);
        ugfx_fill_rect(surf, tx, ty + SLIDER_TRACK_H, 1, SLIDER_TICK_H, s->track_bg);
    }

    uint32_t thumb = s->disabled ? uui_state_bg(s->thumb_bg, UUI_STATE_DISABLED)
                   : (s->dragging || s->hovered)
                       ? uui_state_bg(s->thumb_bg, UUI_STATE_HOVER)
                       : s->thumb_bg;
    ugfx_fill_rect(surf, sx - SLIDER_THUMB_W / 2, ty - SLIDER_THUMB_W / 2,
                    SLIDER_THUMB_W, SLIDER_THUMB_W, thumb);

    // The ring goes round the THUMB, not the whole control: the arrows
    // move the thumb, so that is where the keyboard's cursor is.
    if (s->focused && !s->disabled)
        uui_focus_ring(surf, sx - SLIDER_THUMB_W / 2 - 2, ty - SLIDER_THUMB_W / 2 - 2,
                        SLIDER_THUMB_W + 4, SLIDER_THUMB_W + 4);

    // The CURRENT option's name under the track. Without it the control
    // shows a position and never says what that position means, which
    // for named levels is the whole content.
    const char *label = s->options[sel];
    int ly = s->y + SLIDER_THUMB_W + SLIDER_LABEL_GAP;
    ugfx_draw_string_clipped(surf, s->x + SLIDER_PAD_X, ly,
                              s->w - 2 * SLIDER_PAD_X, label,
                              s->disabled ? uui_state_bg(s->fg, UUI_STATE_DISABLED) : s->fg,
                              s->bg);
}

// --- input ------------------------------------------------------------

int uui_slider_hit(const struct uui_slider *s, int cx, int cy) {
    // The WHOLE widget, label included -- a hit that covered only the
    // track would leave half the control dead to the pointer, which is
    // the trap uui_table shipped with its scrollbar.
    return cx >= s->x && cx < s->x + s->w && cy >= s->y && cy < s->y + s->h;
}

int uui_slider_stop_at(const struct uui_slider *s, int cx) {
    if (s->count <= 1) return 0;
    int x0 = track_x0(s), x1 = track_x1(s);
    if (cx <= x0) return 0;
    if (cx >= x1) return s->count - 1;
    int span = x1 - x0;
    // ROUNDED to the nearest stop, not truncated: truncating makes the
    // last stop reachable only at the exact final pixel.
    return ((cx - x0) * (s->count - 1) + span / 2) / span;
}

int uui_slider_press(struct uui_slider *s, int cx, int cy) {
    if (!uui_slider_hit(s, cx, cy)) return 0;
    int want = uui_slider_stop_at(s, cx);
    s->dragging = 1;
    s->selected = want;
    // NON-ZERO ON ANY HIT, even when the value did not move: the router
    // takes its pointer grab only when press does, and without the grab
    // a drag stops the moment the cursor leaves the thumb.
    return 1;
}

int uui_slider_drag(struct uui_slider *s, int cx, int cy) {
    (void)cy; // a drag tracks x only -- leaving the track vertically
              // must not cancel it, which is how every real slider works
    if (!s->dragging) return 0;
    int want = uui_slider_stop_at(s, cx);
    if (want == s->selected) return 0;
    s->selected = want;
    return 1;
}

void uui_slider_drag_end(struct uui_slider *s) { s->dragging = 0; }

int uui_slider_hover(struct uui_slider *s, int cx, int cy) {
    int on = uui_slider_hit(s, cx, cy);
    if (on == s->hovered) return 0;
    s->hovered = on;
    return 1;
}

int uui_slider_key(struct uui_slider *s, int key) {
    if (s->count <= 0) return 0;
    int before = s->selected;
    switch (key) {
    case KEY_ARROW_LEFT:  if (s->selected > 0) s->selected--; break;
    case KEY_ARROW_RIGHT: if (s->selected < s->count - 1) s->selected++; break;
    case KEY_HOME: s->selected = 0; break;
    case KEY_END:  s->selected = s->count - 1; break;
    default: return 0;
    }
    return s->selected != before;
}

int uui_slider_wheel(struct uui_slider *s, int notches) {
    if (s->count <= 0) return 0;
    int before = s->selected;
    s->selected -= notches; // wheel up = toward the end, as elsewhere
    if (s->selected < 0) s->selected = 0;
    if (s->selected >= s->count) s->selected = s->count - 1;
    return s->selected != before;
}

// --- the ops tables ---------------------------------------------------

static void sl_draw(struct ugfx_surface *surf, const void *w) {
    uui_slider_draw(surf, (const struct uui_slider *)w);
}
static int sl_hit(const void *w, int cx, int cy) {
    return uui_slider_hit((const struct uui_slider *)w, cx, cy);
}
static int sl_press(void *w, int cx, int cy) {
    if (((struct uui_slider *)w)->disabled) return 0;
    return uui_slider_press(w, cx, cy);
}
static int sl_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_slider *s = w;
    if (s->disabled) return 0;
    // The button must still be DOWN -- `dragging` alone is not enough,
    // because the grab outlives any one motion and a button-up motion
    // inside it would move the value to wherever the pointer is. Found
    // on uui_scale, which has the identical shape; 0x1 is the primary
    // button (abi/win_proto.h).
    if (s->dragging && (buttons & 0x1)) return uui_slider_drag(s, cx, cy);
    return uui_slider_hover(s, cx, cy);
}
static int sl_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    if (((struct uui_slider *)w)->disabled) return 0;
    uui_slider_drag_end((struct uui_slider *)w);
    // 1 so the router NAMES this widget to the app -- it reports one
    // only when the widget has a release op, which is how a checkbox
    // once toggled on screen while its app heard nothing.
    return 1;
}
static int sl_wheel(void *w, int notches) { return uui_slider_wheel(w, notches); }
static int sl_key(void *w, int key, unsigned mods) {
    (void)mods;
    if (((struct uui_slider *)w)->disabled) return 0;
    return uui_slider_key(w, key);
}
static void sl_natural(const void *w, int *out_w, int *out_h) {
    uui_slider_natural_size((const struct uui_slider *)w, out_w, out_h);
}
static void sl_geometry(void *w, int x, int y, int width, int height) {
    uui_slider_set_geometry((struct uui_slider *)w, x, y, width, height);
}
static int sl_accepts_focus(const void *w) {
    const struct uui_slider *s = (const struct uui_slider *)w;
    return !s->disabled && s->count > 0;
}
static void sl_set_focused(void *w, int focused) {
    ((struct uui_slider *)w)->focused = focused;
}

const struct uui_widget_ops uui_slider_focus_ops = {
    .key = sl_key,
    .accepts_focus = sl_accepts_focus,
    .set_focused = sl_set_focused,
};

// FILLED AGAINST uui_widget.h, not against a neighbouring widget: three
// tables were found short on 2026-08-19, each missing a slot that failed
// silently and at a distance. See docs/decisions.md.
const struct uui_widget_ops uui_slider_ops = {
    .natural_size  = sl_natural,
    .set_geometry  = sl_geometry,
    .draw          = sl_draw,
    .hit           = sl_hit,
    .press         = sl_press,
    .motion        = sl_motion,
    .release       = sl_release,
    .wheel         = sl_wheel,
    .key           = sl_key,
    .accepts_focus = sl_accepts_focus,
    .set_focused   = sl_set_focused,
};
