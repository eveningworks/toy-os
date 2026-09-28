// See ui/uui_switch.h.
#include "ui/uui_switch.h"
#include "ui/uui_widget.h"
#include "ui/utheme.h"

// The track is two text heights long and one high, so it scales with the
// font like everything else; the knob sits one pixel inside it.
static int track_h(void) { int h = ugfx_char_h(); return h > 10 ? h : 10; }
static int track_w(void) { return track_h() * 2; }
#define SWITCH_TEXT_GAP (ugfx_char_w() / 2 + 2)

static const char *state_text(const struct uui_switch *sw) {
    return sw->on ? sw->on_text : sw->off_text;
}

void uui_switch_init(struct uui_switch *sw, int on) {
    *sw = (struct uui_switch){ .on = on ? 1 : 0, .on_text = "On", .off_text = "Off",
                               .bg = UUI_COLOR_UNSET, .fg = UUI_COLOR_UNSET };
}

// Measured against the WIDER of the two texts, so the switch does not
// change size -- and move its neighbours -- when it is flipped.
void uui_switch_natural_size(const struct uui_switch *sw, int *out_w, int *out_h) {
    int tw = 0;
    if (sw->on_text)  { int w = ugfx_text_width(sw->on_text);  if (w > tw) tw = w; }
    if (sw->off_text) { int w = ugfx_text_width(sw->off_text); if (w > tw) tw = w; }
    *out_w = track_w() + (tw ? SWITCH_TEXT_GAP + tw : 0);
    int h = ugfx_char_h() + 4;
    *out_h = h > track_h() ? h : track_h();
}

void uui_switch_set_geometry(struct uui_switch *sw, int x, int y) {
    sw->x = x;
    sw->y = y;
    uui_switch_natural_size(sw, &sw->w, &sw->h);
}

void uui_switch_draw(struct ugfx_surface *s, const struct uui_switch *sw) {
    uint32_t bg = UUI_COLOR(sw->bg, UTHEME_PANEL_BG);
    uint32_t fg = UUI_COLOR(sw->fg, UTHEME_TEXT);
    uint32_t dim = uui_state_bg(fg, UUI_STATE_DISABLED);
    int tw = track_w(), th = track_h();
    int tx = sw->x, ty = sw->y + (sw->h - th) / 2;

    // ON is a filled accent track with a light knob at the right; OFF is
    // an outlined track with a dark knob at the left -- told apart by
    // shape and lightness, not by hue alone.
    uint32_t track = sw->on ? UTHEME_ACCENT : dim;
    if (sw->disabled) track = sw->on ? dim : uui_state_bg(dim, UUI_STATE_DISABLED);
    else if (sw->hovered) track = uui_state_bg(track, UUI_STATE_HOVER);
    uui_fill_round_rect(s, tx, ty, tw, th, UUI_CAPSULE, track);
    if (!sw->on)
        uui_fill_round_rect(s, tx + 1, ty + 1, tw - 2, th - 2, UUI_CAPSULE, UTHEME_WHITE);

    int k = th - 6;
    int kx = sw->on ? tx + tw - 3 - k : tx + 3;
    uint32_t knob = sw->on ? UTHEME_WHITE : (sw->disabled ? dim : fg);
    uui_fill_round_rect(s, kx, ty + 3, k, k, UUI_CAPSULE, knob);

    const char *t = state_text(sw);
    if (t) {
        int lx = tx + tw + SWITCH_TEXT_GAP;
        ugfx_draw_string_clipped(s, lx, sw->y + (sw->h - ugfx_char_h()) / 2,
                                 sw->x + sw->w - lx, t, sw->disabled ? dim : fg, bg);
    }
    if (sw->focused && !sw->disabled)
        uui_focus_ring(s, tx - 2, ty - 2, tw + 4, th + 4);
}

int uui_switch_hit(const struct uui_switch *sw, int cx, int cy) {
    return uui_hit(sw->x, sw->y, sw->w, sw->h, cx, cy);
}

int uui_switch_toggle(struct uui_switch *sw) {
    if (!sw->disabled) sw->on = !sw->on;
    return sw->on;
}

// --- ops ----------------------------------------------------------------

static void op_natural(const void *w, int *ow, int *oh) { uui_switch_natural_size(w, ow, oh); }
static void op_geometry(void *w, int x, int y, int width, int height) {
    (void)width; (void)height;   // its size is its own
    uui_switch_set_geometry(w, x, y);
}
static void op_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_switch *sw = w;
    *x = sw->x; *y = sw->y; *ow = sw->w; *oh = sw->h;
}
static void op_draw(struct ugfx_surface *s, const void *w) { uui_switch_draw(s, w); }
static int op_hit(const void *w, int cx, int cy) { return uui_switch_hit(w, cx, cy); }
static int op_press(void *w, int cx, int cy, unsigned mods) {
    (void)cx; (void)cy; (void)mods;
    struct uui_switch *sw = w;
    if (sw->disabled) return 0;
    uui_switch_toggle(sw);
    return 1;
}
static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_switch *sw = w;
    int h = !sw->disabled && uui_switch_hit(sw, cx, cy);
    if (h == sw->hovered) return 0;
    sw->hovered = h;
    return 1;
}
// Present so the router names the switch to its app (uui_route.c); the
// toggle already happened on the press.
static int op_release(void *w, int cx, int cy) { (void)w; (void)cx; (void)cy; return 1; }
static int op_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_switch *sw = w;
    if (sw->disabled || key != ' ') return 0;
    uui_switch_toggle(sw);
    return 1;
}
static void op_set_focused(void *w, int f) { ((struct uui_switch *)w)->focused = f; }
static int op_accepts_focus(const void *w) { return !((const struct uui_switch *)w)->disabled; }

const struct uui_widget_ops uui_switch_ops = {
    .natural_size = op_natural,
    .set_geometry = op_geometry,
    .bounds       = op_bounds,
    .draw         = op_draw,
    .hit          = op_hit,
    .press        = op_press,
    .motion       = op_motion,
    .release      = op_release,
    .key          = op_key,
    .set_focused  = op_set_focused,
    .accepts_focus = op_accepts_focus,
};
