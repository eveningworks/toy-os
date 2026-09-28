// See ui/uui_segmented.h.
#include "ui/uui_segmented.h"
#include "ui/uui_widget.h"
#include "ui/uui_describe.h"
#include "ui/utheme.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

static int count_of(const struct uui_segmented *sg) {
    return sg->count < UUI_SEGMENTED_MAX ? sg->count : UUI_SEGMENTED_MAX;
}

static int seg_w(const struct uui_segmented *sg) {
    int widest = 0;
    for (int i = 0; i < count_of(sg); i++) {
        int w = sg->options[i] ? ugfx_text_width(sg->options[i]) : 0;
        if (w > widest) widest = w;
    }
    return widest + 2 * UUI_PAD_X;
}

void uui_segmented_init(struct uui_segmented *sg, const char *const *options,
                        int count, int selected) {
    *sg = (struct uui_segmented){ .options = options, .count = count,
                                  .selected = selected, .hovered = -1, .armed = -1,
                                  .bg = UUI_COLOR_UNSET, .fg = UUI_COLOR_UNSET };
}

void uui_segmented_natural_size(const struct uui_segmented *sg, int *out_w, int *out_h) {
    *out_w = count_of(sg) * seg_w(sg);
    *out_h = ugfx_char_h() + 8;
}

void uui_segmented_set_geometry(struct uui_segmented *sg, int x, int y) {
    sg->x = x;
    sg->y = y;
    uui_segmented_natural_size(sg, &sg->w, &sg->h);
}

int uui_segmented_at(const struct uui_segmented *sg, int cx, int cy) {
    if (!uui_hit(sg->x, sg->y, sg->w, sg->h, cx, cy)) return -1;
    int i = (cx - sg->x) / seg_w(sg);
    return i < count_of(sg) ? i : -1;
}

void uui_segmented_draw(struct ugfx_surface *s, const struct uui_segmented *sg) {
    uint32_t fg = UUI_COLOR(sg->fg, UTHEME_TEXT);
    uint32_t face = UTHEME_WHITE;
    uint32_t edge = uui_state_bg(fg, UUI_STATE_DISABLED);
    if (sg->disabled) fg = edge;
    int sw = seg_w(sg), n = count_of(sg);

    // The outer capsule-ish frame, then each face inset by the 1px edge
    // so neighbours share one divider line.
    uui_fill_round_rect(s, sg->x, sg->y, sg->w, sg->h, 4, edge);
    uui_fill_round_rect(s, sg->x + 1, sg->y + 1, sg->w - 2, sg->h - 2, 3, face);
    for (int i = 0; i < n; i++) {
        int x = sg->x + i * sw;
        int chosen = i == sg->selected;
        uint32_t bg = chosen ? (sg->disabled ? edge : UTHEME_ACCENT) : face;
        if (!sg->disabled && !chosen) {
            if (i == sg->armed) bg = uui_state_bg(face, UUI_STATE_PRESSED);
            else if (i == sg->hovered) bg = uui_state_bg(face, UUI_STATE_HOVER);
        }
        if (bg != face) {
            // An END segment rounds its OUTER corners only: a rounded
            // fill, then its inner half squared off again.
            int l = i == 0, r = i == n - 1, fy = sg->y + 1, fh = sg->h - 2;
            if (l || r) {
                uui_fill_round_rect(s, x + l, fy, sw - l - r, fh, 3, bg);
                if (l && !r) ugfx_fill_rect(s, x + sw / 2, fy, sw - sw / 2, fh, bg);
                if (r && !l) ugfx_fill_rect(s, x, fy, sw / 2, fh, bg);
            } else {
                ugfx_fill_rect(s, x, fy, sw, fh, bg);
            }
        }
        if (i > 0 && !chosen && i - 1 != sg->selected)
            ugfx_fill_rect(s, x, sg->y + 4, 1, sg->h - 8, edge);
        const char *t = sg->options[i] ? sg->options[i] : "";
        int tw = ugfx_text_width(t);
        int tx = x + (sw - tw) / 2;
        if (tx < x + 2) tx = x + 2;
        ugfx_draw_string_clipped(s, tx, sg->y + (sg->h - ugfx_char_h()) / 2, sw - 4, t,
                                 chosen && !sg->disabled ? UTHEME_ACCENT_TEXT : fg, bg);
    }
    // Round the WHOLE control, outside it: the chosen segment is
    // accent-filled, so a ring on it would not show (focusring_test).
    if (sg->focused && !sg->disabled)
        uui_focus_ring(s, sg->x - 2, sg->y - 2, sg->w + 4, sg->h + 4);
}

// --- ops ----------------------------------------------------------------

static void op_natural(const void *w, int *ow, int *oh) { uui_segmented_natural_size(w, ow, oh); }
static void op_geometry(void *w, int x, int y, int width, int height) {
    (void)width; (void)height;   // its size is its own
    uui_segmented_set_geometry(w, x, y);
}
static void op_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_segmented *sg = w;
    *x = sg->x; *y = sg->y; *ow = sg->w; *oh = sg->h;
}
static void op_draw(struct ugfx_surface *s, const void *w) { uui_segmented_draw(s, w); }
static int op_hit(const void *w, int cx, int cy) { return uui_segmented_at(w, cx, cy) >= 0; }

static int op_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_segmented *sg = w;
    if (sg->disabled) return 0;
    sg->armed = uui_segmented_at(sg, cx, cy);
    return sg->armed >= 0;
}
static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_segmented *sg = w;
    int h = sg->disabled ? -1 : uui_segmented_at(sg, cx, cy);
    if (h == sg->hovered) return 0;
    sg->hovered = h;
    return 1;
}
// Chosen only if released over the segment that was pressed -- dragging
// off cancels, as it does a button.
static int op_release(void *w, int cx, int cy) {
    struct uui_segmented *sg = w;
    int at = uui_segmented_at(sg, cx, cy);
    if (sg->armed >= 0 && at == sg->armed) sg->selected = at;
    sg->armed = -1;
    return 1;
}
static int op_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_segmented *sg = w;
    int n = count_of(sg), to = sg->selected;
    if (sg->disabled || n <= 0) return 0;
    switch (key) {
    case KEY_ARROW_LEFT:  to = to > 0 ? to - 1 : 0; break;
    case KEY_ARROW_RIGHT: to = to < n - 1 ? to + 1 : n - 1; break;
    case KEY_HOME:        to = 0; break;
    case KEY_END:         to = n - 1; break;
    default:              return 0;
    }
    if (to < 0) to = 0;
    sg->selected = to;
    return 1;
}
static void op_set_focused(void *w, int f) { ((struct uui_segmented *)w)->focused = f; }
static int op_accepts_focus(const void *w) {
    const struct uui_segmented *sg = w;
    return !sg->disabled && sg->count > 0;
}
static void op_describe(const void *w, const struct uui_describe *d) {
    const struct uui_segmented *sg = w;
    int sw = seg_w(sg);
    for (int i = 0; i < count_of(sg); i++)
        uui_describe_rect_i(d, "slot", i, sg->x + i * sw, sg->y, sw, sg->h);
    uui_describe_int(d, "selected", sg->selected);
}

const struct uui_widget_ops uui_segmented_ops = {
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
    .describe     = op_describe,
};
