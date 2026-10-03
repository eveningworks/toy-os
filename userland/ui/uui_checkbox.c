// checkbox. See ui/uui_checkbox.h.
#include "ui/uui_checkbox.h"
#include "ui/uui_widget.h"  // the ops table at the bottom of this file
#include "ui/utheme.h"      // utheme_indicator() -- the box's default size
#include "keyboard.h"       // KEY_* codes, as delivered by WIN_EV_KEY

#define CHECKBOX_LABEL_GAP 6

// The box's drawn edge. `cb->size` when the caller chose one, else the
// FONT HEIGHT -- resolved HERE (measure/draw time) rather than baked at
// init, because ugfx_char_h() is 0 until uapp_run() has fetched the
// font, so a size computed in an app's main() comes out zero and the box
// draws and hit-tests as nothing (it looked exactly like a dead control,
// and cost two apps the same workaround before this). GTK and Qt size a
// checkbox indicator from the font too, so "0 means the font height" is
// the expected default, not a guess. Never returns 0.
static int box_size(const struct uui_checkbox *cb) {
    return cb->size > 0 ? cb->size : utheme_indicator();
}

void uui_checkbox_natural_size(const struct uui_checkbox *cb, int *out_w, int *out_h) {
    int bs = box_size(cb);
    if (out_w) {
        *out_w = cb->label ? bs + CHECKBOX_LABEL_GAP + ugfx_text_width(cb->label) : bs;
    }
    // The box or the text, whichever is taller.
    if (out_h) *out_h = bs > ugfx_char_h() ? bs : ugfx_char_h();
}

void uui_checkbox_set_geometry(struct uui_checkbox *cb, int x, int y) {
    cb->x = x;
    cb->y = y;
    uui_checkbox_natural_size(cb, &cb->w, &cb->h);
}

void uui_checkbox_init(struct uui_checkbox *cb, int x, int y, int size,
                        const char *label, uint32_t bg, uint32_t fg) {
    cb->size = size;
    cb->label = label;
    cb->bg = bg;
    cb->fg = fg;
    cb->checked = 0;
    cb->hovered = 0;
    cb->focused = 0;
    // OFF by default -- see uui_checkbox.h and the exception recorded in
    // docs/gui-guidelines.md.
    cb->hover_effect = 0;
    cb->disabled = 0;
    uui_checkbox_set_geometry(cb, x, y);
}

void uui_checkbox_draw(struct ugfx_surface *s, const struct uui_checkbox *cb) {
    // See uui_radio_list.c: UUI_COLOR_UNSET means "the theme's", and a
    // widget that passes it straight to uui_state_bg() draws black.
    uint32_t bg = UUI_COLOR(cb->bg, UTHEME_PANEL_BG);
    uint32_t base_fg = UUI_COLOR(cb->fg, UTHEME_TEXT);
    uint32_t fg = cb->disabled ? uui_state_bg(base_fg, UUI_STATE_DISABLED)
                               : base_fg;

    if (cb->hovered && cb->hover_effect && !cb->disabled) {
        // The WHOLE clickable area -- the hit test is box+label, and a
        // highlight smaller than its target misleads about where to
        // click. w/h ARE the natural size, so the two cannot drift.
        bg = uui_state_bg(bg, UUI_STATE_HOVER);
        ugfx_fill_rect(s, cb->x, cb->y, cb->w, cb->h, bg);
    }

    int bs = box_size(cb);
    if (cb->accent) {
        int r = bs / 4 > 1 ? bs / 4 : 1;
        uint32_t edge = cb->checked && !cb->disabled ? UTHEME_ACCENT : UTHEME_OUTLINE;
        uui_fill_round_rect(s, cb->x, cb->y, bs, bs, r, edge);
        if (!cb->checked || cb->disabled)
            uui_fill_round_rect(s, cb->x + 1, cb->y + 1, bs - 2, bs - 2, r - 1 > 0 ? r - 1 : 0, UTHEME_WHITE);
        if (cb->checked) {
            uint32_t tick = cb->disabled ? fg : UTHEME_ACCENT_TEXT;
            int x0 = cb->x + bs * 2 / 9, y0 = cb->y + bs / 2, x1 = cb->x + bs * 4 / 9, y1 = cb->y + bs * 7 / 10;
            int x2 = cb->x + bs * 7 / 9, y2 = cb->y + bs * 3 / 10;
            for (int k = 0; k < 2; k++) {
                ugfx_draw_line(s, x0, y0 + k, x1, y1 + k, tick, GEOM_AA);
                ugfx_draw_line(s, x1, y1 + k, x2, y2 + k, tick, GEOM_AA);
            }
        }
    } else {
        ugfx_draw_rect(s, cb->x, cb->y, bs, bs, fg);
    }
    if (cb->checked && !cb->accent) {
        int inset = bs / 4 > 0 ? bs / 4 : 1;
        ugfx_fill_rect(s, cb->x + inset, cb->y + inset,
                        bs - 2 * inset, bs - 2 * inset, fg);
    }
    if (cb->label) {
        ugfx_draw_string_clipped(s, cb->x + bs + CHECKBOX_LABEL_GAP,
                                  cb->y + (bs - ugfx_char_h()) / 2,
                                  cb->w - bs - CHECKBOX_LABEL_GAP,
                                  cb->label, fg, bg);
    }

    // The focus indicator LAST, so it sits over the label rather than
    // under it, and around the whole clickable area -- which is what
    // Space acts on.
    if (cb->focused && !cb->disabled) {
        uui_focus_ring(s, cb->x - 2, cb->y - 2, cb->w + 4, cb->h + 4);
    }
}

int uui_checkbox_hit(const struct uui_checkbox *cb, int cx, int cy) {
    // One geometry, shared by the draw, the hover wash and the hit test.
    return uui_hit(cb->x, cb->y, cb->w, cb->h, cx, cy);
}

int uui_checkbox_hover(struct uui_checkbox *cb, int cx, int cy) {
    int hit = !cb->disabled && uui_checkbox_hit(cb, cx, cy);
    if (cb->hovered == hit) return 0;
    cb->hovered = hit;
    return 1;
}

int uui_checkbox_toggle(struct uui_checkbox *cb) {
    if (cb->disabled) return cb->checked;
    cb->checked = !cb->checked;
    return cb->checked;
}

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// Act-on-contact, which is correct here and not a shortcut: the result
// of a checkbox is visible the instant it happens, so there is nothing
// for a commit-on-release rule to protect (see docs/gui-guidelines.md).
static int cb_ops_hit(const void *w, int cx, int cy) {
    return uui_checkbox_hit((const struct uui_checkbox *)w, cx, cy);
}

static int cb_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    (void)cx; (void)cy;
    struct uui_checkbox *cb = (struct uui_checkbox *)w;
    if (cb->disabled) return 0;
    uui_checkbox_toggle(cb);
    return 1;
}

static int cb_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    return uui_checkbox_hover((struct uui_checkbox *)w, cx, cy);
}

static void cb_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_checkbox_draw(s, (const struct uui_checkbox *)w);
}

static void cb_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_checkbox_natural_size((const struct uui_checkbox *)w, out_w, out_h);
}

static void cb_ops_set_geometry(void *w, int x, int y, int width, int height) {
    (void)width; (void)height; // its size is its own -- see the header
    uui_checkbox_set_geometry((struct uui_checkbox *)w, x, y);
}

int uui_checkbox_key(struct uui_checkbox *cb, int key) {
    if (cb->disabled || key != ' ') return 0;
    uui_checkbox_toggle(cb);
    return 1;
}

// A NO-OP THAT RETURNS 1, and it is not decoration: the router only
// names a widget to the app when that widget HAS a release op
// (uui_route.c), so without this a checkbox in a routed layout toggled
// on screen and the app was never told. It toggles on PRESS -- there is
// no partial state for a commit-on-release rule to protect -- so there
// is nothing to do here except be present.
static int cb_ops_release(void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return 1;
}

// NATURAL_SIZE, SET_GEOMETRY AND RELEASE WERE ALL MISSING until
// 2026-08-19, so a checkbox declared in a uui_layout was never
// positioned, never measured, and never reported to its app. Every
// function it needed already existed; only the table was short -- the
// same gap uui_dropdown_ops had, found the same day. See
// docs/decisions.md on the ops table being the contract.
static int cb_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_checkbox_key((struct uui_checkbox *)w, key);
}

static void cb_ops_set_focused(void *w, int focused) {
    ((struct uui_checkbox *)w)->focused = focused;
}

// A checkbox always accepts focus. There is no "empty" state to refuse
// for, unlike a listbox with no rows -- an unchecked box is still a
// control with something to do.
static int cb_ops_accepts_focus(const void *w) { return !((const struct uui_checkbox *)w)->disabled; }

static void checkbox_bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_checkbox *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

const struct uui_widget_ops uui_checkbox_ops = {
    .bounds = checkbox_bounds_op,
    .natural_size = cb_ops_natural_size,
    .set_geometry = cb_ops_set_geometry,
    .release = cb_ops_release,
    .draw   = cb_ops_draw,
    .hit    = cb_ops_hit,
    .press  = cb_ops_press,
    .motion = cb_ops_motion,
    .key    = cb_ops_key,
    .set_focused  = cb_ops_set_focused,
    .accepts_focus = cb_ops_accepts_focus,
};
