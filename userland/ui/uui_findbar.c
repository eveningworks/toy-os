// The find control. See ui/uui_findbar.h for the contract.
#include "ui/uui_findbar.h"
#include "ui/uui_primitives.h"
#include "ui/uui_describe.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"
#include "lib/icon_cache.h"
#include "keyboard.h"
#include <stdio.h>
#include <string.h>

// Every measurement is font-derived (docs/gui-guidelines.md).
static int pad(void)   { return ugfx_char_w() / 2 + 2; }
static int btn_w(void) { return ugfx_char_h() + 4; }
static int lens_w(void) { return ugfx_char_h() + 2; }
// "999 of 999" is the widest the readout gets; "No results" is close.
static int count_w(void) { return ugfx_text_width("No results") + ugfx_char_w(); }

void uui_findbar_init(struct uui_findbar *f) {
    memset(f, 0, sizeof *f);
    uui_textbox_init(&f->field, "");
    f->field.placeholder = "Find";
    f->hovered = f->pressed = -1;
}

// The pieces, left to right: lens, field, readout, up, down, close. ONE
// derivation, which draw, hit and the test rects all call.
static void layout(const struct uui_findbar *f, int *fx, int *fw, int *cx, int *bx) {
    int p = pad();
    int right = f->x + f->w - p;
    *bx = right - UUI_FB_BUTTONS * btn_w();
    *cx = *bx - count_w();
    *fx = f->x + p + lens_w();
    *fw = *cx - p - *fx;
    if (*fw < ugfx_char_w() * 4) *fw = ugfx_char_w() * 4;
}

void uui_findbar_set_geometry(struct uui_findbar *f, int x, int y, int w, int h) {
    f->x = x; f->y = y; f->w = w; f->h = h;
    int fx, fw, cx, bx;
    layout(f, &fx, &fw, &cx, &bx);
    int fh = utheme_control_h();
    uui_textbox_set_geometry(&f->field, fx, y + (h - fh) / 2, fw, fh);
}

void uui_findbar_natural_size(const struct uui_findbar *f, int *out_w, int *out_h) {
    (void)f;
    *out_w = ugfx_char_w() * 22 + lens_w() + count_w() + UUI_FB_BUTTONS * btn_w() + 3 * pad();
    *out_h = utheme_control_h() + 2 * pad();
}

int uui_findbar_button_rect(const struct uui_findbar *f, int which,
                            int *x, int *y, int *w, int *h) {
    if (which < 0 || which >= UUI_FB_BUTTONS) return 0;
    int fx, fw, cx, bx;
    layout(f, &fx, &fw, &cx, &bx);
    *w = btn_w();
    *h = btn_w();
    *x = bx + which * btn_w();
    *y = f->y + (f->h - *h) / 2;
    return 1;
}

static int button_at(const struct uui_findbar *f, int px, int py) {
    for (int i = 0; i < UUI_FB_BUTTONS; i++) {
        int x, y, w, h;
        uui_findbar_button_rect(f, i, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, px, py)) return i;
    }
    return -1;
}

void uui_findbar_activate(struct uui_findbar *f) {
    uui_textbox_set_active(&f->field, 1);
    uui_textbox_select(&f->field, 0, f->field.len);
}

const char *uui_findbar_query(const struct uui_findbar *f) {
    return uui_textbox_text(&f->field);
}

enum uui_findbar_event uui_findbar_take(struct uui_findbar *f) {
    enum uui_findbar_event e = (enum uui_findbar_event)f->event;
    f->event = UUI_FIND_NONE;
    return e;
}

int uui_findbar_key(struct uui_findbar *f, int key, unsigned mods) {
    if (key == 0x1B) { f->event = UUI_FIND_CLOSE; return 1; }
    if (key == '\n' || key == '\r') {
        f->event = (mods & KEY_MOD_SHIFT) ? UUI_FIND_PREV : UUI_FIND_NEXT;
        return 1;
    }
    // CHANGED is decided by comparing the text, not by the return value:
    // an arrow key is consumed and changes nothing worth searching again.
    char before[UUI_TEXTBOX_MAX];
    strlcpy(before, f->field.buf, sizeof before);
    if (!uui_textbox_key_mods(&f->field, key, mods)) return 0;
    if (strcmp(before, f->field.buf) != 0) f->event = UUI_FIND_CHANGED;
    return 1;
}

// --- drawing -------------------------------------------------------------

static void draw_chevron(struct ugfx_surface *s, int cx, int cy, int up, uint32_t c) {
    int r = ugfx_char_h() / 5 + 1;
    int dy = up ? -r / 2 : r / 2;
    for (int t = 0; t < 2; t++) {
        ugfx_draw_line(s, cx - r, cy - dy + t, cx, cy + dy + t, c, GEOM_AA);
        ugfx_draw_line(s, cx, cy + dy + t, cx + r, cy - dy + t, c, GEOM_AA);
    }
}

static void draw_cross(struct ugfx_surface *s, int cx, int cy, uint32_t c) {
    int r = ugfx_char_h() / 5;
    for (int t = 0; t < 2; t++) {
        ugfx_draw_line(s, cx - r + t, cy - r, cx + r + t, cy + r, c, GEOM_AA);
        ugfx_draw_line(s, cx + r + t, cy - r, cx - r + t, cy + r, c, GEOM_AA);
    }
}

static void fb_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_findbar *f = w;
    uint32_t bg = UTHEME_CHROME;
    if (f->pill) {
        int r = ugfx_char_h() / 2;
        uui_fill_round_rect(s, f->x, f->y, f->w, f->h, r, UTHEME_OUTLINE);
        uui_fill_round_rect(s, f->x + 1, f->y + 1, f->w - 2, f->h - 2, r - 1, bg);
    } else {
        ugfx_fill_rect(s, f->x, f->y, f->w, f->h, bg);
        ugfx_fill_rect(s, f->x, f->y, f->w, 1, UTHEME_SEPARATOR);
    }

    int fx, fw, cx, bx;
    layout(f, &fx, &fw, &cx, &bx);
    int mid = f->y + f->h / 2;

    // The lens: the icon set's when it is there, in the VIEW role.
    const struct uimg *ico = icon_get("tb-find", ugfx_char_h());
    if (ico)
        ugfx_blit_tinted(s, f->x + pad(), mid - ico->h / 2, ico->w, ico->h,
                         ico->px, ico->w, utheme_action(UTHEME_ACT_VIEW));

    uui_textbox_draw(s, &f->field);

    // The readout says nothing until there is a query to report on.
    char cnt[24] = "";
    if (f->field.len) {
        if (f->matches == 0)        strlcpy(cnt, "No results", sizeof cnt);
        else if (f->current > 0)    snprintf(cnt, sizeof cnt, "%d of %d", f->current, f->matches);
        else                        snprintf(cnt, sizeof cnt, "%d found", f->matches);
    }
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    ugfx_draw_string_clipped(s, cx + ugfx_char_w() / 2, mid - ugfx_char_h() / 2,
                             count_w() - ugfx_char_w() / 2, cnt,
                             f->matches == 0 ? utheme_action(UTHEME_ACT_DANGER) : dim, bg);

    for (int i = 0; i < UUI_FB_BUTTONS; i++) {
        int x, y, bw, bh;
        uui_findbar_button_rect(f, i, &x, &y, &bw, &bh);
        if (f->pressed == i || f->hovered == i) {
            enum uui_state st = f->pressed == i ? UUI_STATE_PRESSED : UUI_STATE_HOVER;
            uui_fill_round_rect(s, x + 1, y + 1, bw - 2, bh - 2, ugfx_char_h() / 4,
                                uui_state_bg(bg, st));
        }
        int ccx = x + bw / 2, ccy = y + bh / 2;
        int none = f->matches == 0;
        uint32_t nav = none ? dim : utheme_action(UTHEME_ACT_NAV);
        if (i == UUI_FB_PREV)      draw_chevron(s, ccx, ccy, 1, nav);
        else if (i == UUI_FB_NEXT) draw_chevron(s, ccx, ccy, 0, nav);
        else                       draw_cross(s, ccx, ccy, UTHEME_TEXT);
    }
}

// --- input ---------------------------------------------------------------

static int fb_hit(const void *w, int cx, int cy) {
    const struct uui_findbar *f = w;
    return uui_hit(f->x, f->y, f->w, f->h, cx, cy);
}

static int fb_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_findbar *f = w;
    f->pressed = button_at(f, cx, cy);
    if (f->pressed < 0 && uui_textbox_hit(&f->field, cx, cy)) {
        uui_textbox_set_active(&f->field, 1);
        f->field.ed.cursor = uui_textbox_index_at_x(&f->field, cx);
        f->field.ed.sel_active = 0;
    }
    return 1;
}

static int fb_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_findbar *f = w;
    int h = button_at(f, cx, cy);
    if (h == f->hovered) return 0;
    f->hovered = h;
    return 1;
}

// A button commits on RELEASE over the button it was pressed on, like
// every other commit in the toolkit (docs/gui-guidelines.md).
static int fb_release(void *w, int cx, int cy) {
    struct uui_findbar *f = w;
    int was = f->pressed;
    f->pressed = -1;
    if (was < 0 || button_at(f, cx, cy) != was) return was >= 0;
    f->event = was == UUI_FB_PREV ? UUI_FIND_PREV
             : was == UUI_FB_NEXT ? UUI_FIND_NEXT : UUI_FIND_CLOSE;
    return 1;
}

static int fb_cursor(const void *w, int cx, int cy) {
    const struct uui_findbar *f = w;
    return uui_textbox_hit(&f->field, cx, cy) ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT;
}

static void ops_natural(const void *w, int *ow, int *oh) { uui_findbar_natural_size(w, ow, oh); }
static void ops_geometry(void *w, int x, int y, int ww, int hh) { uui_findbar_set_geometry(w, x, y, ww, hh); }
static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_findbar *f = w;
    *x = f->x; *y = f->y; *ow = f->w; *oh = f->h;
}

static void ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_findbar *f = w;
    static const char *const names[UUI_FB_BUTTONS] = { "prev", "next", "close" };
    for (int i = 0; i < UUI_FB_BUTTONS; i++) {
        int x, y, bw, bh;
        uui_findbar_button_rect(f, i, &x, &y, &bw, &bh);
        uui_describe_rect(d, names[i], x, y, bw, bh);
    }
    uui_describe_rect(d, "field", f->field.x, f->field.y, f->field.w, f->field.h);
    uui_describe_int(d, "matches", f->matches);
    uui_describe_int(d, "current", f->current);
}

// The inner field's, for the shared edit menu (ui/uui_widget.h).
static int findbar_edit_target(void *w, int cx, int cy, struct uui_edit_target *out) {
    struct uui_findbar *p = (struct uui_findbar *)w;
    return uui_textbox_edit_target(&p->field, cx, cy, out);
}

const struct uui_widget_ops uui_findbar_ops = {
    .natural_size = ops_natural,
    .set_geometry = ops_geometry,
    .bounds       = ops_bounds,
    .draw         = fb_draw,
    .hit          = fb_hit,
    .press        = fb_press,
    .motion       = fb_motion,
    .release      = fb_release,
    .cursor       = fb_cursor,
    .describe     = ops_describe,
    .edit_target = findbar_edit_target,
};
