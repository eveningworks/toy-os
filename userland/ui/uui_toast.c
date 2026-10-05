// See ui/uui_toast.h.
#include "ui/uui_toast.h"
#include "ui/uui_anim.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "lib/utween.h"
#include "fixed.h"
#include <string.h>

void uui_toast_show(struct uui_toast *t, const char *text, unsigned in_ms,
                    unsigned long long t0_ns) {
    strlcpy(t->text, text, sizeof t->text);
    t->in_ms = in_ms;
    t->t0_ns = t0_ns;
    t->shown = 1;
}

void uui_toast_hide(struct uui_toast *t) { t->shown = 0; }

int uui_toast_rect(const struct uui_toast *t, int x, int y, int w, int h,
                   int *rx, int *ry, int *rw, int *rh) {
    if (!t->shown) return 0;
    int ch = ugfx_char_h();
    int pw = ugfx_text_width(t->text) + 2 * ch;
    int ph = ch + ch / 2 + 6;
    if (pw > w - 8) pw = w - 8;
    *rw = pw;
    *rh = ph;
    *rx = x + (w - pw) / 2;
    *ry = y + h - ph - ch;
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
    // Inverted from the window's own colours, so it reads on any content
    // and still follows the theme.
    uui_fill_round_rect(s, px, py, pw, ph, UUI_CAPSULE, UTHEME_TEXT);
    int tw = ugfx_text_width(t->text);
    int pad = ugfx_char_h();
    int tx = px + (pw - tw) / 2;
    if (tx < px + pad) tx = px + pad;
    ugfx_draw_string_clipped(s, tx, py + (ph - ugfx_char_h()) / 2,
                             px + pw - pad - tx, t->text, UTHEME_WHITE, UTHEME_TEXT);
    ugfx_clip_restore(s, &saved);

    if (moving) uui_anim_request();
    return moving;
}
