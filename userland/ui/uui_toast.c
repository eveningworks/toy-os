// See ui/uui_toast.h.
#include "ui/uui_toast.h"
#include "ui/uui_anim.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "ui/uui_popup.h"
#include "lib/icon_cache.h"
#include "lib/utween.h"
#include "fixed.h"
#include <string.h>

void uui_toast_show(struct uui_toast *t, const char *text, unsigned in_ms,
                    unsigned long long t0_ns) {
    strlcpy(t->text, text, sizeof t->text);
    t->action[0] = '\0';
    t->in_ms = in_ms;
    t->t0_ns = t0_ns;
    t->shown = 1;
}

void uui_toast_hide(struct uui_toast *t) { t->shown = 0; }

void uui_toast_set_action(struct uui_toast *t, const char *label) {
    strlcpy(t->action, label ? label : "", sizeof t->action);
}

// The action slot's width: its label on a button-sized chip.
static int action_w(const struct uui_toast *t) {
    return t->action[0] ? ugfx_text_width(t->action) + 2 * ugfx_char_h() : 0;
}

static int card(const struct uui_toast *t) { return t->style == UUI_TOAST_CARD; }
static int icon_w(const struct uui_toast *t) { return card(t) && t->icon ? ugfx_char_h() + 6 : 0; }
static int close_w(const struct uui_toast *t) { return card(t) && t->closable ? ugfx_char_h() + 8 : 0; }

int uui_toast_rect(const struct uui_toast *t, int x, int y, int w, int h,
                   int *rx, int *ry, int *rw, int *rh) {
    if (!t->shown) return 0;
    int ch = ugfx_char_h();
    int aw = action_w(t);
    int pw = ugfx_text_width(t->text) + 2 * ch + (aw ? aw + ch / 2 : 0) + icon_w(t) + close_w(t);
    int ph = card(t) ? ch * 2 + 8 : ch + ch / 2 + 6 + (aw ? 6 : 0);
    if (pw > w - 8) pw = w - 8;
    *rw = pw;
    *rh = ph;
    // The pill centres on the box; the card keeps to its corner.
    *rx = card(t) ? x + w - pw - ch : x + (w - pw) / 2;
    *ry = y + h - ph - ch;
    return 1;
}

int uui_toast_close_rect(const struct uui_toast *t, int x, int y, int w, int h,
                         int *rx, int *ry, int *rw, int *rh) {
    int px, py, pw, ph;
    if (!close_w(t) || !uui_toast_rect(t, x, y, w, h, &px, &py, &pw, &ph)) return 0;
    *rw = close_w(t) - 4;
    *rh = *rw;
    *rx = px + pw - close_w(t);
    *ry = py + (ph - *rh) / 2;
    return 1;
}

int uui_toast_action_rect(const struct uui_toast *t, int x, int y, int w, int h,
                          int *rx, int *ry, int *rw, int *rh) {
    int px, py, pw, ph;
    if (!t->action[0] || !uui_toast_rect(t, x, y, w, h, &px, &py, &pw, &ph)) return 0;
    *rw = action_w(t);
    *rh = card(t) ? ugfx_char_h() + 10 : ph - 8;
    *rx = px + pw - *rw - (card(t) ? close_w(t) + 2 : 4);
    *ry = py + (ph - *rh) / 2;
    return 1;
}

int uui_toast_draw(struct ugfx_surface *s, struct uui_toast *t,
                   int x, int y, int w, int h, unsigned long long now_ns) {
    int px, py, pw, ph;
    if (!uui_toast_rect(t, x, y, w, h, &px, &py, &pw, &ph)) return 0;
    if (now_ns < t->t0_ns) { uui_anim_request(); return 1; }

    // Rises from just below the box's bottom edge, eased out.
    int moving = 0;
    int drop = (y + h) - py;
    if (t->in_ms) {
        unsigned long long el = (now_ns - t->t0_ns) / 1000000ull;
        if (el < t->in_ms) {
            fx_t p = (fx_t)((long long)el * FX_ONE / t->in_ms);
            fx_t e = utween_ease_out(p);
            py += (int)(((long long)drop * (FX_ONE - e)) >> FX_SHIFT);
            moving = 1;
        }
    }

    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, x, y, w, h);
    int tw = ugfx_text_width(t->text);
    int pad = ugfx_char_h();
    int aw = action_w(t);
    int room = pw - (aw ? aw + pad / 2 : 0) - close_w(t);   // the text's share
    uint32_t bg = UTHEME_TEXT, fg = UTHEME_WHITE;
    int tx = aw ? px + pad : px + (pw - tw) / 2;
    if (card(t)) {
        // A menu's surface and hairline, over a soft shadow drawn as a
        // few widening rings of glass -- the window has no compositor
        // shadow to borrow for something inside it.
        int r = uui_popup_radius();
        for (int k = 4; k >= 1; k--)
            uui_glass_round_rect(s, px - k, py - k + 3, pw + 2 * k, ph + 2 * k, r + k,
                                 ugfx_rgb(0, 0, 0), 9, 0);
        bg = uui_popup_bg();
        fg = UTHEME_TEXT;
        uui_fill_round_rect(s, px, py, pw, ph, r, uui_popup_border());
        uui_fill_round_rect(s, px + 1, py + 1, pw - 2, ph - 2, r - 1, bg);
        tx = px + pad * 3 / 4;
        if (icon_w(t)) {
            const struct uimg *ico = icon_get(t->icon, ugfx_char_h());
            if (ico) ugfx_blit_alpha(s, tx, py + (ph - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
            tx += icon_w(t);
        }
    } else {
        // Inverted from the window's own colours, so it reads on any
        // content and still follows the theme.
        uui_fill_round_rect(s, px, py, pw, ph, UUI_CAPSULE, bg);
    }
    if (tx < px + pad / 2) tx = px + pad / 2;
    ugfx_draw_string_clipped(s, tx, py + (ph - ugfx_char_h()) / 2,
                             px + room - pad / 2 - tx, t->text, fg, bg);
    ugfx_clip_restore(s, &saved);

    if (moving) uui_anim_request();
    return moving;
}
