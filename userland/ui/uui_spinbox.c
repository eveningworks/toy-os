// spinbox -- see ui/uui_spinbox.h for the contract, and for why typing
// does not change the value until it is committed.
#include "ui/uui_spinbox.h"
#include "ui/uui_widget.h"
#include <stdio.h>
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// The steppers occupy a column on the right, split in half.
#define STEP_W 14

static int step_of(const struct uui_spinbox *s) {
    return s->step > 0 ? s->step : 1;
}

static int clamp(const struct uui_spinbox *s, int v) {
    if (v < s->min) return s->min;
    if (v > s->max) return s->max;
    return v;
}

// Renders `value` into the field. The field is the DISPLAY of the value,
// never the other way round -- the value is authoritative and the text
// is regenerated from it whenever it changes.
static void render(struct uui_spinbox *s) {
    char buf[24];
    snprintf(buf, sizeof buf, "%d", s->value);
    uui_textbox_init(&s->field, buf);
}

// Parses the field. Returns 1 and writes the number, or 0 for anything
// that is not a plain integer -- deliberately NOT tolerant: "12abc" is a
// typo, and taking the 12 out of it means acting on something the user
// did not type.
static int parse_field(const struct uui_spinbox *s, int *out) {
    const char *p = s->field.buf;
    if (!p || !*p) return 0;
    int neg = (*p == '-');
    if (neg) p++;
    if (!*p) return 0;
    int v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 1000000) return 0;
    }
    *out = neg ? -v : v;
    return 1;
}

void uui_spinbox_init(struct uui_spinbox *s, int value,
                      int min, int max, int step, const char *unit) {
    s->x = s->y = s->w = s->h = 0;
    s->min = min;
    s->max = max > min ? max : min + 1;
    s->step = step > 0 ? step : 1;
    s->unit = unit;
    s->value = clamp(s, value);
    s->hovered = 0;
    s->armed = 0;
    s->bg = ugfx_rgb(255, 255, 255);
    s->fg = ugfx_rgb(20, 20, 20);
    s->border = ugfx_rgb(170, 172, 180);
    s->step_bg = ugfx_rgb(238, 238, 242);
    render(s);
}

void uui_spinbox_set_value(struct uui_spinbox *s, int value) {
    s->value = clamp(s, value);
    render(s);
}

int uui_spinbox_value(const struct uui_spinbox *s) { return s->value; }

int uui_spinbox_commit(struct uui_spinbox *s) {
    int v = 0;
    // REVERT, DO NOT CLAMP. Clamping turns a typed 500 into 300 and
    // reports success, so the user is told they got what they asked for
    // and did not. Putting the old number back says plainly that the
    // entry was refused -- and the old number is a value they already
    // had, so nothing is lost.
    if (!parse_field(s, &v) || v < s->min || v > s->max) {
        render(s);
        return 0;
    }
    int changed = (v != s->value);
    s->value = v;
    render(s); // normalises "0150" to "150"
    return changed;
}

static int bump(struct uui_spinbox *s, int dir) {
    int v = clamp(s, s->value + dir * step_of(s));
    if (v == s->value) return 0; // already at the end of the range
    s->value = v;
    render(s);
    return 1;
}

// --- geometry -----------------------------------------------------------

// Wide enough for the widest number the range allows PLUS the unit, so
// the box does not resize as the value changes -- measured over the
// bounds rather than over the current value, the same rule
// uui_slider_natural_size() follows. See CLAUDE.md on natural_size.
void uui_spinbox_natural_size(const struct uui_spinbox *s, int *out_w, int *out_h) {
    char lo[24], hi[24];
    snprintf(lo, sizeof lo, "%d", s->min);
    snprintf(hi, sizeof hi, "%d", s->max);
    int w = ugfx_text_width(lo);
    int wh = ugfx_text_width(hi);
    if (wh > w) w = wh;
    if (s->unit && s->unit[0]) w += ugfx_text_width(s->unit) + ugfx_char_w() / 2;
    if (out_w) *out_w = w + ugfx_char_w() * 2 + STEP_W;
    if (out_h) *out_h = ugfx_char_h() + 10;
}

void uui_spinbox_set_geometry(struct uui_spinbox *s, int x, int y, int w, int h) {
    s->x = x; s->y = y; s->w = w; s->h = h;
    // The field owns everything left of the stepper column. It is told
    // its geometry rather than deriving it, so the caret and the
    // click-to-position arithmetic inside uui_textbox stay that widget's
    // business.
    int fw = w - STEP_W;
    uui_textbox_set_geometry(&s->field, x, y, fw > 0 ? fw : 0, h);
}

static int in_steppers(const struct uui_spinbox *s, int cx, int cy) {
    return cx >= s->x + s->w - STEP_W && cx < s->x + s->w &&
           cy >= s->y && cy < s->y + s->h;
}

// 1 = up, 2 = down, 0 = not on a stepper.
static int stepper_at(const struct uui_spinbox *s, int cx, int cy) {
    if (!in_steppers(s, cx, cy)) return 0;
    return (cy < s->y + s->h / 2) ? 1 : 2;
}

// --- drawing ------------------------------------------------------------

// A small filled triangle, drawn rather than spelled with a character so
// it does not depend on the font having one (the atlas is 101 glyphs --
// see docs/conventions/gui.md).
//
// `cy` is the TOP of the glyph and rows run downward for both, which is
// what makes the direction a property of the WIDTH sequence and nothing
// else: an up arrow starts narrow (its apex) and widens, a down arrow
// starts wide and narrows to its apex. The first version varied the y
// direction instead and kept the width sequence the same, which drew
// both triangles the wrong way up -- symmetrically, so they looked like
// a matched pair and read as deliberate.
static void arrow(struct ugfx_surface *surf, int cx, int cy, int up, uint32_t col) {
    for (int i = 0; i < 4; i++) {
        int wdt = up ? 1 + 2 * i : 7 - 2 * i;
        if (wdt <= 0) break;
        ugfx_fill_rect(surf, cx - wdt / 2, cy + i, wdt, 1, col);
    }
}

void uui_spinbox_draw(struct ugfx_surface *surf, const struct uui_spinbox *s) {
    // The field draws its own background, caret and selection -- this
    // must not paint over it, which is why only the stepper column and
    // the border are drawn here.
    uui_textbox_draw(surf, &s->field);

    // THE UNIT IS DRAWN ONLY WHEN NOT EDITING. While the field has the
    // caret it must show exactly what will be parsed; a "%" sitting
    // after the digits would read as part of the text the user is
    // typing, and they would try to delete it.
    if (s->unit && s->unit[0] && !s->field.active) {
        int tw = ugfx_text_width(s->field.buf);
        int ux = s->field.x + 4 + tw + ugfx_char_w() / 2;
        int uy = s->field.y + (s->field.h - ugfx_char_h()) / 2;
        int avail = s->x + s->w - STEP_W - ux - 2;
        if (avail > 0)
            ugfx_draw_string_clipped(surf, ux, uy, avail, s->unit,
                                      ugfx_rgb(120, 122, 130), s->bg);
    }

    int sx = s->x + s->w - STEP_W;
    int half = s->h / 2;
    for (int i = 0; i < 2; i++) {
        int up = (i == 0);
        int ry = up ? s->y : s->y + half;
        int rh = up ? half : s->h - half;
        int which = up ? 1 : 2;
        // A stepper at the end of its range is drawn DISABLED, because a
        // control that looks live and does nothing is worse than one
        // that says it cannot.
        int usable = up ? (s->value < s->max) : (s->value > s->min);
        uint32_t bg = s->step_bg;
        if (usable && s->armed == which) bg = uui_state_bg(s->step_bg, UUI_STATE_PRESSED);
        else if (usable && s->hovered == which) bg = uui_state_bg(s->step_bg, UUI_STATE_HOVER);
        ugfx_fill_rect(surf, sx, ry, STEP_W, rh, bg);
        // Centred on the half: the glyph is 4 rows tall and drawn
        // downward from `cy`, so the top is half its height above the
        // middle -- the same expression for both, now that direction is
        // carried by the widths rather than by the y step.
        arrow(surf, sx + STEP_W / 2, ry + rh / 2 - 2, up,
              usable ? s->fg : ugfx_rgb(180, 182, 190));
    }
    ugfx_draw_rect(surf, s->x, s->y, s->w, s->h, s->border);
    ugfx_fill_rect(surf, sx, s->y, 1, s->h, s->border);
}

// --- input --------------------------------------------------------------

int uui_spinbox_hit(const struct uui_spinbox *s, int cx, int cy) {
    return cx >= s->x && cx < s->x + s->w && cy >= s->y && cy < s->y + s->h;
}

int uui_spinbox_press(struct uui_spinbox *s, int cx, int cy) {
    if (!uui_spinbox_hit(s, cx, cy)) return 0;
    int st = stepper_at(s, cx, cy);
    if (st) {
        // ARMED, NOT APPLIED (docs/gui-guidelines.md): dragging off the
        // stepper before letting go cancels it. Committing on press
        // would make a mis-click unrepeatable-but-already-done.
        s->armed = st;
        // Leaving the field means whatever was typed has to be adopted
        // or reverted NOW -- otherwise a stepper press would step from a
        // stale value while the field shows a different number.
        if (s->field.active) { s->field.active = 0; uui_spinbox_commit(s); }
        return 1;
    }
    s->field.active = 1;
    return uui_textbox_hit(&s->field, cx, cy) ? 1 : 1;
}

int uui_spinbox_motion(struct uui_spinbox *s, int cx, int cy, unsigned buttons) {
    (void)buttons;
    int h = stepper_at(s, cx, cy);
    if (h == s->hovered) return 0;
    s->hovered = h;
    return 1;
}

int uui_spinbox_release(struct uui_spinbox *s, int cx, int cy) {
    int armed = s->armed;
    s->armed = 0;
    if (!armed) return 0;
    // Only if the pointer is still ON the stepper it was pressed on.
    if (stepper_at(s, cx, cy) != armed) return 1;
    return bump(s, armed == 1 ? +1 : -1) ? 1 : 1;
}

int uui_spinbox_wheel(struct uui_spinbox *s, int notches) {
    if (!notches) return 0;
    return bump(s, notches > 0 ? +1 : -1);
}

int uui_spinbox_key(struct uui_spinbox *s, int key, unsigned mods) {
    if (key == KEY_ARROW_UP)   return bump(s, +1);
    if (key == KEY_ARROW_DOWN) return bump(s, -1);
    if (key == '\n' || key == '\r') {
        // ENTER ADOPTS. Returning 1 whether or not the value changed:
        // the field may still have been normalised ("0150" -> "150"),
        // and the app wants a repaint either way.
        uui_spinbox_commit(s);
        return 1;
    }
    // Everything else is the field's -- including every editing key,
    // which is the whole reason a uui_textbox is embedded rather than
    // reimplemented.
    return uui_textbox_key_mods(&s->field, key, mods);
}

// --- the ops table ------------------------------------------------------

static void draw_op(struct ugfx_surface *surf, const void *w) {
    uui_spinbox_draw(surf, (const struct uui_spinbox *)w);
}
static int hit_op(const void *w, int cx, int cy) {
    return uui_spinbox_hit((const struct uui_spinbox *)w, cx, cy);
}
static int press_op(void *w, int cx, int cy) { return uui_spinbox_press(w, cx, cy); }
static int motion_op(void *w, int cx, int cy, unsigned b) {
    return uui_spinbox_motion(w, cx, cy, b);
}
static int release_op(void *w, int cx, int cy) { return uui_spinbox_release(w, cx, cy); }
static int wheel_op(void *w, int notches) { return uui_spinbox_wheel(w, notches); }
static int key_op(void *w, int key, unsigned mods) { return uui_spinbox_key(w, key, mods); }
static void natural_op(const void *w, int *out_w, int *out_h) {
    uui_spinbox_natural_size((const struct uui_spinbox *)w, out_w, out_h);
}
static void set_geometry_op(void *w, int x, int y, int rw, int rh) {
    uui_spinbox_set_geometry(w, x, y, rw, rh);
}
// It takes keys, so it says whether it wants focus -- always yes: a
// spinbox with no value to edit is not a state it has.
static int accepts_focus_op(const void *w) { (void)w; return 1; }

static void spinbox_bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_spinbox *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

const struct uui_widget_ops uui_spinbox_ops = {
    .bounds = spinbox_bounds_op,
    .draw = draw_op,
    .accepts_focus = accepts_focus_op,
    .hit = hit_op,
    .press = press_op,
    .motion = motion_op,
    .release = release_op,
    .wheel = wheel_op,
    .key = key_op,
    .natural_size = natural_op,
    .set_geometry = set_geometry_op,
};
