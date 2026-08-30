// scale -- see ui/uui_scale.h for why this is not uui_slider.
#include "ui/uui_scale.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include <stddef.h>

// Everything here is FONT-DERIVED (docs/gui-guidelines.md): the thumb is
// a text row tall, so the control grows with the session font instead of
// staying a 10px sliver on a large one.
static int thumb_sz(void) {
    int h = ugfx_char_h();
    return h < 8 ? 8 : h;
}
static int track_th(void) {
    int t = ugfx_char_h() / 4;
    return t < 3 ? 3 : t;
}

void uui_scale_init(struct uui_scale *s, long min, long max, long value) {
    s->x = s->y = s->w = s->h = 0;
    s->min = min;
    s->max = max > min ? max : min;
    s->value = value;
    s->step = s->page = 0;
    s->hovered = s->dragging = s->focused = s->disabled = 0;
    // From the THEME, never hand-picked (docs/gui-guidelines.md).
    s->track_bg = UTHEME_BUTTON_BG;
    s->fill_bg  = UTHEME_ACCENT;
    s->thumb_bg = UTHEME_ACCENT;
    uui_scale_set_value(s, value);
}

void uui_scale_set_range(struct uui_scale *s, long min, long max) {
    s->min = min;
    s->max = max > min ? max : min;
    uui_scale_set_value(s, s->value);
}

void uui_scale_set_value(struct uui_scale *s, long value) {
    if (value < s->min) value = s->min;
    if (value > s->max) value = s->max;
    s->value = value;
}

long uui_scale_value(const struct uui_scale *s) { return s->value; }

static long span_of(const struct uui_scale *s) { return s->max - s->min; }

static long step_of(const struct uui_scale *s) {
    if (s->step > 0) return s->step;
    long d = span_of(s) / 100;
    return d > 0 ? d : 1;
}
static long page_of(const struct uui_scale *s) {
    if (s->page > 0) return s->page;
    return step_of(s) * 10;
}

// --- geometry ---------------------------------------------------------

static int pad(void) { return thumb_sz() / 2; }
static int track_x0(const struct uui_scale *s) { return s->x + pad(); }
static int track_x1(const struct uui_scale *s) { return s->x + s->w - pad(); }
static int track_y(const struct uui_scale *s) { return s->y + thumb_sz() / 2; }

static int thumb_x(const struct uui_scale *s) {
    int x0 = track_x0(s), x1 = track_x1(s);
    long span = span_of(s);
    if (span <= 0 || x1 <= x0) return x0;
    return x0 + (int)(((long long)(s->value - s->min) * (x1 - x0)) / span);
}

void uui_scale_natural_size(const struct uui_scale *s, int *out_w, int *out_h) {
    (void)s;
    // Eight thumbs of travel is the floor at which dragging means
    // anything; a layout with room to spare gives it more.
    if (out_w) *out_w = thumb_sz() * 8;
    if (out_h) *out_h = thumb_sz();
}

void uui_scale_set_geometry(struct uui_scale *s, int x, int y, int w, int h) {
    s->x = x; s->y = y; s->w = w;
    // The height is the control's own, as uui_slider's is: a track and a
    // thumb stretched vertically is a gap, not a bigger control.
    int nh = thumb_sz();
    s->h = (h > 0 && h < nh) ? h : nh;
}

// --- drawing ----------------------------------------------------------

void uui_scale_draw(struct ugfx_surface *surf, const struct uui_scale *s) {
    int x0 = track_x0(s), x1 = track_x1(s);
    if (x1 <= x0) return;
    int ty = track_y(s), th = track_th();
    int tx = thumb_x(s);
    int sz = thumb_sz();

    uint32_t track = s->disabled ? uui_state_bg(s->track_bg, UUI_STATE_DISABLED) : s->track_bg;
    uint32_t fill  = s->disabled ? uui_state_bg(s->fill_bg,  UUI_STATE_DISABLED) : s->fill_bg;

    ugfx_fill_rect(surf, x0, ty - th / 2, x1 - x0, th, track);
    if (tx > x0) ugfx_fill_rect(surf, x0, ty - th / 2, tx - x0, th, fill);

    uint32_t thumb = s->disabled ? uui_state_bg(s->thumb_bg, UUI_STATE_DISABLED)
                   : (s->dragging || s->hovered)
                       ? uui_state_bg(s->thumb_bg, UUI_STATE_HOVER)
                       : s->thumb_bg;
    ugfx_fill_rect(surf, tx - sz / 2, ty - sz / 2, sz, sz, thumb);

    // Round the THUMB, as uui_slider does: the arrows move the thumb, so
    // that is where the keyboard's cursor is.
    if (s->focused && !s->disabled)
        uui_focus_ring(surf, tx - sz / 2 - 2, ty - sz / 2 - 2, sz + 4, sz + 4);
}

// --- input ------------------------------------------------------------

int uui_scale_hit(const struct uui_scale *s, int cx, int cy) {
    return uui_hit(s->x, s->y, s->w, s->h, cx, cy);
}

long uui_scale_value_at(const struct uui_scale *s, int cx) {
    int x0 = track_x0(s), x1 = track_x1(s);
    long span = span_of(s);
    if (span <= 0 || x1 <= x0) return s->min;
    if (cx <= x0) return s->min;
    if (cx >= x1) return s->max;
    // ROUNDED, not truncated -- truncating makes the top of the range
    // reachable only on its one final pixel.
    long long num = (long long)(cx - x0) * span + (x1 - x0) / 2;
    return s->min + (long)(num / (x1 - x0));
}

int uui_scale_press(struct uui_scale *s, int cx, int cy) {
    if (s->disabled || !uui_scale_hit(s, cx, cy)) return 0;
    // A click anywhere on the track JUMPS there, as on every real scale
    // -- the alternative (page towards the click) is a Win32 trackbar
    // behaviour nothing else has kept.
    s->dragging = 1;
    uui_scale_set_value(s, uui_scale_value_at(s, cx));
    return 1;
}

int uui_scale_drag(struct uui_scale *s, int cx, int cy) {
    (void)cy;
    if (s->disabled || !s->dragging) return 0;
    long v = uui_scale_value_at(s, cx);
    if (v == s->value) return 0;
    uui_scale_set_value(s, v);
    return 1;
}

void uui_scale_drag_end(struct uui_scale *s) { s->dragging = 0; }

int uui_scale_hover(struct uui_scale *s, int cx, int cy) {
    int on = !s->disabled && uui_scale_hit(s, cx, cy);
    if (on == s->hovered) return 0;
    s->hovered = on;
    return 1;
}

int uui_scale_wheel(struct uui_scale *s, int notches) {
    if (s->disabled || !notches) return 0;
    long before = s->value;
    uui_scale_set_value(s, s->value + (long)notches * step_of(s));
    return s->value != before;
}

int uui_scale_key(struct uui_scale *s, int key) {
    if (s->disabled) return 0;
    long before = s->value;
    switch (key) {
    case KEY_ARROW_LEFT:  case KEY_ARROW_DOWN:  uui_scale_set_value(s, s->value - step_of(s)); break;
    case KEY_ARROW_RIGHT: case KEY_ARROW_UP:    uui_scale_set_value(s, s->value + step_of(s)); break;
    case KEY_PAGE_DOWN:   uui_scale_set_value(s, s->value - page_of(s)); break;
    case KEY_PAGE_UP:     uui_scale_set_value(s, s->value + page_of(s)); break;
    case KEY_HOME:        uui_scale_set_value(s, s->min); break;
    case KEY_END:         uui_scale_set_value(s, s->max); break;
    default: return 0;
    }
    return s->value != before || 1;
}

// --- the ops tables ---------------------------------------------------

static void sc_draw(struct ugfx_surface *surf, const void *w) {
    uui_scale_draw(surf, (const struct uui_scale *)w);
}
static void sc_natural(const void *w, int *out_w, int *out_h) {
    uui_scale_natural_size((const struct uui_scale *)w, out_w, out_h);
}
static void sc_geometry(void *w, int x, int y, int width, int height) {
    uui_scale_set_geometry((struct uui_scale *)w, x, y, width, height);
}
static void sc_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_scale *s = w;
    if (x) *x = s->x;
    if (y) *y = s->y;
    if (out_w) *out_w = s->w;
    if (out_h) *out_h = s->h;
}
static int sc_hit(const void *w, int cx, int cy) {
    return uui_scale_hit((const struct uui_scale *)w, cx, cy);
}
static int sc_press(void *w, int cx, int cy, unsigned mods) { return uui_scale_press(w, cx, cy); }
static int sc_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_scale *s = w;
    // A DRAG NEEDS THE BUTTON STILL DOWN. `dragging` alone is not
    // enough: the widget holds the pointer grab from its press until
    // its release, and a motion can arrive inside that window with
    // nothing held -- which then moves the value to wherever the
    // pointer happens to be. Measured: a click on the right of a
    // volume scale set it to 100 on the press and a button-up motion
    // dragged it back to 0 before the release arrived. uui_button
    // consults `buttons` for the same reason.
    if (s->dragging && (buttons & 0x1))   // 0x1 = primary; abi/win_proto.h
        return uui_scale_drag(s, cx, cy);
    return uui_scale_hover(s, cx, cy);
}
static int sc_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    uui_scale_drag_end((struct uui_scale *)w);
    // 1 so the router NAMES this widget to the app: it reports one only
    // when the widget has a release op.
    return 1;
}
static int sc_wheel(void *w, int notches) { return uui_scale_wheel(w, notches); }
static int sc_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_scale_key(w, key);
}
static int sc_accepts_focus(const void *w) {
    const struct uui_scale *s = w;
    return !s->disabled && s->max > s->min;
}
static void sc_set_focused(void *w, int focused) {
    ((struct uui_scale *)w)->focused = focused;
}

const struct uui_widget_ops uui_scale_focus_ops = {
    .key = sc_key,
    .accepts_focus = sc_accepts_focus,
    .set_focused = sc_set_focused,
};

// FILLED AGAINST uui_widget.h, not against the widget this was modelled
// on: a copied table inherits its gaps.
const struct uui_widget_ops uui_scale_ops = {
    .natural_size  = sc_natural,
    .set_geometry  = sc_geometry,
    .bounds        = sc_bounds,
    .draw          = sc_draw,
    .hit           = sc_hit,
    .press         = sc_press,
    .motion        = sc_motion,
    .release       = sc_release,
    .wheel         = sc_wheel,
    .key           = sc_key,
    .accepts_focus = sc_accepts_focus,
    .set_focused   = sc_set_focused,
};
