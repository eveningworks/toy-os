// A single button's state. Split out of uui.c -- see ui/uui_button.h.
#include "ui/uui_button.h"

void uui_button_init(struct uui_button *b, int x, int y, int w, int h,
                      const char *label, uint32_t bg, uint32_t fg, int code) {
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->label = label;
    b->bg = bg; b->fg = fg;
    b->code = code;
    b->pressed = 0;
    b->hovered = 0;
    b->disabled = 0;
    b->focused = 0;
    b->outlined = 0;
}

void uui_button_natural_size(const struct uui_button *b, int *out_w, int *out_h) {
    if (out_w) *out_w = ugfx_text_width(b->label) + 2 * UUI_PAD_X;
    if (out_h) *out_h = ugfx_char_h() + 2 * UUI_PAD_Y;
}

void uui_button_set_geometry(struct uui_button *b, int x, int y, int w, int h) {
    b->x = x; b->y = y; b->w = w; b->h = h;
}

void uui_button_draw_one(struct ugfx_surface *s, const struct uui_button *b) {
    // Order matters: pressed wins over hovered. A pressed button is
    // always also under the cursor, and showing the hover wash on top
    // of the press would weaken the stronger signal.
    enum uui_state st = UUI_STATE_REST;
    if (b->disabled)     st = UUI_STATE_DISABLED;
    else if (b->pressed) st = UUI_STATE_PRESSED;
    else if (b->hovered) st = UUI_STATE_HOVER;
    if (!b->outlined) {
        uui_button_draw(s, b->x, b->y, b->w, b->h, b->label, b->bg, b->fg, st);
        return;
    }
    // The border is the face darkened twice, so it follows the face's own
    // colour -- grey on a plain button, deep blue on an accent one.
    uint32_t face = uui_state_bg(b->bg, st);
    uint32_t edge = uui_state_bg(uui_state_bg(b->bg, UUI_STATE_PRESSED), UUI_STATE_PRESSED);
    if (st == UUI_STATE_DISABLED) edge = uui_state_bg(edge, UUI_STATE_DISABLED);
    uui_fill_round_rect(s, b->x, b->y, b->w, b->h, 4, edge);
    uui_fill_round_rect(s, b->x + 1, b->y + 1, b->w - 2, b->h - 2, 3, face);
    uui_button_draw_label(s, b->x, b->y, b->w, b->h, b->label, b->fg, face, st);
}

// --- as a layout child ------------------------------------------------
//
// This USED to have no `draw` slot, on the theory that a button is
// painted by its GROUP and the layout only places it. That was wrong,
// and wrong in the worst way: Calculator's twenty buttons were placed,
// hit-tested and never drawn. It shipped, because every test asserted
// that clicking a button CHANGED THE DISPLAY -- which it did. "A click
// has an effect" is not "the button is visible", and nothing was
// checking the second.

static void btn_natural(const void *w, int *out_w, int *out_h) {
    uui_button_natural_size((const struct uui_button *)w, out_w, out_h);
}
static void btn_geometry(void *w, int x, int y, int width, int height) {
    uui_button_set_geometry((struct uui_button *)w, x, y, width, height);
}
static int btn_hit(const void *w, int cx, int cy) {
    const struct uui_button *b = (const struct uui_button *)w;
    return uui_hit(b->x, b->y, b->w, b->h, cx, cy);
}

static void btn_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_button *b = w;
    uui_button_draw_one(s, b);
    // OUTSIDE the face, as uui_checkbox's is: an accent ring on an
    // accent-filled button (a primary action) would not show at all.
    if (b->focused && !b->disabled) uui_focus_ring(s, b->x - 2, b->y - 2, b->w + 4, b->h + 4);
}

// --- routed pointer input, so a LONE button works --------------------
//
// A single button used to draw, hit-test, and ignore clicks entirely:
// routing lived only in uui_button_group, so even one button needed a
// group wrapped round it to do anything. That is not how the toolkits
// this is modelled on behave -- a QPushButton and a Win32 BUTTON both
// handle their own click, and Qt's QButtonGroup exists for EXCLUSIVITY,
// not for delivering the press. It is also a silent trap: the button
// draws perfectly, hit-tests correctly and does nothing, with nothing to
// point at the cause (it cost this project a debugging cycle).
//
// Additive: the group still routes its own buttons, so a grid like
// Calculator's is unchanged. What this buys is that a button no longer
// NEEDS one.
//
// Arm on press, commit on release, and a press dragged off commits
// nothing -- docs/gui-guidelines.md's rule, implemented the same way the
// group implements it rather than a second time with different edges.
static int btn_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_button *b = (struct uui_button *)w;
    if (b->disabled) return 0;
    int hit = uui_hit(b->x, b->y, b->w, b->h, cx, cy);
    if (b->pressed == hit) return 0;
    b->pressed = hit;
    return hit;
}

static int btn_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_button *b = (struct uui_button *)w;
    if (b->disabled) return 0;
    int hit = uui_hit(b->x, b->y, b->w, b->h, cx, cy);
    // Held: re-arm against the CURRENT position, so dragging off
    // disarms and dragging back re-arms.
    int *flag = buttons ? &b->pressed : &b->hovered;
    if (*flag == hit) return 0;
    *flag = hit;
    return 1;
}

static int btn_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_button *b = (struct uui_button *)w;
    // A press dragged off already had `pressed` cleared by btn_motion,
    // so this correctly commits nothing.
    int was = b->pressed;
    b->pressed = 0;
    return was; // 1 = committed, and the app is told which widget by id
}

// KEYBOARD: a focused button presses on Space or Enter, as a Win32 and a
// Qt push button do. A key has no drag to cancel, so it commits at once;
// the router then names the button to its app with UUI_REASON_KEY. A
// disabled button refuses focus -- the ring steps over it -- because a
// NULL accepts_focus means "yes", and the ring used to park on disabled
// buttons that could neither show the focus nor act on it.
static int btn_key(void *w, int key, unsigned mods) {
    (void)mods;
    const struct uui_button *b = w;
    return !b->disabled && (key == ' ' || key == '\n' || key == '\r');
}
static void btn_set_focused(void *w, int f) { ((struct uui_button *)w)->focused = f; }
static int btn_accepts_focus(const void *w) { return !((const struct uui_button *)w)->disabled; }

static void button_bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_button *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

const struct uui_widget_ops uui_button_ops = {
    .bounds = button_bounds_op,
    .natural_size = btn_natural,
    .set_geometry = btn_geometry,
    .draw         = btn_draw,
    .hit          = btn_hit,
    .press        = btn_press,
    .motion       = btn_motion,
    .release      = btn_release,
    .key          = btn_key,
    .set_focused  = btn_set_focused,
    .accepts_focus = btn_accepts_focus,
};
