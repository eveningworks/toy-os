// See uui_nametpl.h. Three rows, from the font: the field, the chips,
// the preview line.
#include "ui/uui_nametpl.h"
#include <stdio.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

static int gap(void)    { return ugfx_char_h() / 2; }
static int chip_h(void) { return ugfx_char_h() + ugfx_char_h() / 2; }
static int chip_pad(void) { return ugfx_char_h() * 2 / 3; }

static int field_h(const struct uui_nametpl *t) {
    int w, h;
    uui_textbox_natural_size(&t->field, &w, &h);
    return h;
}

static int chip_w(const struct uui_nametpl_chip *c) {
    char t[40];
    snprintf(t, sizeof t, "+ %s", c->label);
    return ugfx_text_width(t) + 2 * chip_pad();
}

// Chip `i`'s rect; they run left to right under the field and stop at
// the widget's width (one row: a template has a handful of tokens).
static int chip_rect(const struct uui_nametpl *t, int i, int *x, int *y, int *w, int *h) {
    int cx = t->x, cy = t->y + field_h(t) + gap();
    for (int k = 0; k < t->chip_count && k < UUI_NAMETPL_CHIPS; k++) {
        int cw = chip_w(&t->chips[k]);
        if (cx + cw > t->x + t->w && k > 0) return 0;
        if (k == i) { *x = cx; *y = cy; *w = cw; *h = chip_h(); return 1; }
        cx += cw + gap();
    }
    return 0;
}

static int chip_at(const struct uui_nametpl *t, int px, int py) {
    for (int i = 0; i < t->chip_count; i++) {
        int x, y, w, h;
        if (chip_rect(t, i, &x, &y, &w, &h) && uui_hit(x, y, w, h, px, py)) return i;
    }
    return -1;
}

void uui_nametpl_init(struct uui_nametpl *t, const char *initial,
                      const struct uui_nametpl_chip *chips, int chip_count,
                      const struct unametpl_var *sample, int sample_count) {
    memset(t, 0, sizeof *t);
    uui_textbox_init(&t->field, initial);
    t->chips = chips;
    t->chip_count = chip_count > UUI_NAMETPL_CHIPS ? UUI_NAMETPL_CHIPS : chip_count;
    t->sample = sample;
    t->sample_count = sample_count;
    t->pressed = -1;
}

const char *uui_nametpl_text(const struct uui_nametpl *t) { return uui_textbox_text(&t->field); }

int uui_nametpl_valid(const struct uui_nametpl *t) {
    return unametpl_valid(uui_nametpl_text(t), t->sample, t->sample_count);
}

// --- ops --------------------------------------------------------------

static void ops_natural_size(const void *w, int *ow, int *oh) {
    const struct uui_nametpl *t = w;
    int chips = 0;
    for (int i = 0; i < t->chip_count; i++) chips += chip_w(&t->chips[i]) + (i ? gap() : 0);
    *ow = chips;
    *oh = field_h(t) + gap() + chip_h() + gap() + ugfx_char_h();
}

static void ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_nametpl *t = w;
    t->x = x; t->y = y; t->w = width; t->h = height;
    uui_textbox_set_geometry(&t->field, x, y, width, field_h(t));
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_nametpl *t = w;
    *x = t->x; *y = t->y; *ow = t->w; *oh = t->h;
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_nametpl *t = w;
    uui_textbox_draw(s, &t->field);
    uint32_t ground = UTHEME_WINDOW_BG;
    for (int i = 0; i < t->chip_count; i++) {
        int x, y, cw, ch;
        if (!chip_rect(t, i, &x, &y, &cw, &ch)) break;
        uint32_t edge = UTHEME_OUTLINE;
        uint32_t fill = t->disabled ? uui_state_bg(UTHEME_BUTTON_BG, UUI_STATE_DISABLED)
                      : t->pressed == i ? uui_state_bg(UTHEME_BUTTON_BG, UUI_STATE_PRESSED)
                      : UTHEME_BUTTON_BG;
        uui_fill_round_rect(s, x, y, cw, ch, UUI_CAPSULE, edge);
        uui_fill_round_rect(s, x + 1, y + 1, cw - 2, ch - 2, UUI_CAPSULE, fill);
        char lab[40];
        snprintf(lab, sizeof lab, "+ %s", t->chips[i].label);
        ugfx_draw_string_clipped(s, x + chip_pad(), y + (ch - ugfx_char_h()) / 2, cw - 2 * chip_pad(),
                                 lab, UTHEME_TEXT, fill);
    }
    // The preview: what this template names a file today.
    char name[128], line[160];
    int ok = unametpl_expand(uui_nametpl_text(t), t->sample, t->sample_count, name, sizeof name);
    if (ok) snprintf(line, sizeof line, "Next file: %s%s", name, t->suffix ? t->suffix : "");
    else snprintf(line, sizeof line, "Not a usable name: use the tokens below the field, no '/'");
    int py = t->y + field_h(t) + gap() + chip_h() + gap();
    ugfx_draw_string_clipped(s, t->x, py, t->w, line,
                             ok ? UTHEME_TEXT : utheme_action(UTHEME_ACT_DANGER), ground);
}

static int ops_hit(const void *w, int cx, int cy) {
    const struct uui_nametpl *t = w;
    if (t->disabled) return 0;
    return uui_textbox_hit(&t->field, cx, cy) || chip_at(t, cx, cy) >= 0;
}

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_nametpl *t = w;
    if (t->disabled) return 0;
    t->in_field = 0;
    t->pressed = chip_at(t, cx, cy);
    if (t->pressed >= 0) return 1;
    if (!uui_textbox_hit(&t->field, cx, cy)) return 0;
    t->in_field = 1;
    return uui_textbox_ops.press(&t->field, cx, cy, mods);
}

static int ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_nametpl *t = w;
    if (t->in_field) return uui_textbox_ops.motion(&t->field, cx, cy, buttons);
    return 0;
}

// A chip types its token on RELEASE over the same chip, so a press can
// still be dragged off and cancelled.
static int ops_release(void *w, int cx, int cy) {
    struct uui_nametpl *t = w;
    int c = t->pressed;
    t->pressed = -1;
    t->in_field = 0;
    if (c >= 0 && chip_at(t, cx, cy) == c) {
        char tok[40];
        snprintf(tok, sizeof tok, "<%s>", t->chips[c].token);
        uui_textbox_insert(&t->field, tok);
    }
    return 1;
}

static int ops_key(void *w, int key, unsigned mods) {
    struct uui_nametpl *t = w;
    if (t->disabled) return 0;
    return uui_textbox_key_mods(&t->field, key, mods);
}

static void ops_set_focused(void *w, int focused) {
    uui_textbox_set_active(&((struct uui_nametpl *)w)->field, focused);
}

static int ops_accepts_focus(const void *w) { return !((const struct uui_nametpl *)w)->disabled; }

static int ops_cursor(const void *w, int cx, int cy) {
    const struct uui_nametpl *t = w;
    return uui_textbox_hit(&t->field, cx, cy) ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT;
}

const struct uui_widget_ops uui_nametpl_ops = {
    .natural_size  = ops_natural_size,
    .set_geometry  = ops_set_geometry,
    .bounds        = ops_bounds,
    .draw          = ops_draw,
    .hit           = ops_hit,
    .press         = ops_press,
    .motion        = ops_motion,
    .release       = ops_release,
    .key           = ops_key,
    .set_focused   = ops_set_focused,
    .accepts_focus = ops_accepts_focus,
    .cursor        = ops_cursor,
};
