// The shortcut capture control. See ui/uui_keycapture.h for what it is
// and why the compositor has to stand down while it listens.
#include "ui/uui_keycapture.h"
#include "ui/uui_widget.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "keyboard.h"
#include <string.h>

#define PROMPT "Press the new shortcut"
#define UNBOUND "Disabled"
// WHAT THIS CONTROL IS SIZED FROM: the widest thing it can ever be asked
// to show, as a CONSTANT. Not the current value -- a natural size that
// depends on state is a feedback loop between layout and measurement
// (CLAUDE.md) and makes the box jump as you aim at it -- and not the
// prompt alone, which was the first version and clipped
// "Shift+Super+S, Print Screen" to "Shift+Super+S, Print S".
//
// A single combination cannot exceed KEYCOMBO_TEXT_MAX, and a value may
// list two; this is a hair wider than the longest pair the defaults
// carry, which is the honest bound rather than an exact one.
#define WIDEST "Ctrl+Alt+Shift+Super+Print Screen "

void uui_keycapture_init(struct uui_keycapture *k, const char *value) {
    memset(k, 0, sizeof *k);
    uui_keycapture_set(k, value);
}

void uui_keycapture_set(struct uui_keycapture *k, const char *value) {
    // Round-tripped through the parser rather than copied, so whatever
    // is stored is SHOWN CANONICALLY -- a hand-edited "win+e" in
    // /etc/shortcuts.conf reads as "Super+E" here (api/keycombo.h).
    struct keycombo c;
    if (value && keycombo_parse(value, &c) &&
        keycombo_format(&c, k->text, sizeof k->text))
        return;
    strlcpy(k->text, value ? value : "", sizeof k->text);
}

static void disarm(struct uui_keycapture *k) {
    if (!k->armed) return;
    k->armed = 0;
    if (k->on_arm) k->on_arm(k->ctx, 0);
}

void uui_keycapture_cancel(struct uui_keycapture *k) {
    if (!k->armed) return;
    disarm(k);
    if (k->on_done) k->on_done(k->ctx, 0);
}

// --- as a widget ------------------------------------------------------

static void kc_natural_size(const void *w, int *out_w, int *out_h) {
    (void)w;
    // CONSTANT, never the current value -- see WIDEST above.
    int prompt = ugfx_text_width(PROMPT);
    int widest = ugfx_text_width(WIDEST);
    if (out_w) *out_w = (widest > prompt ? widest : prompt) + 2 * UUI_PAD_X;
    if (out_h) *out_h = ugfx_char_h() + 2 * UUI_PAD_Y;
}

static void kc_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_keycapture *k = w;
    k->x = x; k->y = y; k->w = width; k->h = height;
}

static void kc_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_keycapture *k = w;
    if (x) *x = k->x;
    if (y) *y = k->y;
    if (out_w) *out_w = k->w;
    if (out_h) *out_h = k->h;
}

static void kc_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_keycapture *k = w;
    enum uui_state st = k->armed   ? UUI_STATE_PRESSED
                      : k->hovered ? UUI_STATE_HOVER
                                   : UUI_STATE_REST;
    uint32_t bg = uui_state_bg(UTHEME_WHITE, st);
    ugfx_fill_rect(s, k->x, k->y, k->w, k->h, bg);
    ugfx_draw_rect(s, k->x, k->y, k->w, k->h,
                   k->focused ? UTHEME_ACCENT : UTHEME_BORDER);

    const char *text = k->armed     ? PROMPT
                     : k->text[0]   ? k->text
                                    : UNBOUND;
    // CLIPPED, always: this sits in a settings row whose width is the
    // layout's to decide, and gfx_draw_string() does not clip
    // (docs/gui-guidelines.md -- the same overlap bug twice).
    int ty = k->y + (k->h - ugfx_char_h()) / 2;
    ugfx_draw_string_clipped(s, k->x + UUI_PAD_X, ty, k->w - 2 * UUI_PAD_X,
                              text, UTHEME_TEXT, bg);
}

static int kc_hit(const void *w, int cx, int cy) {
    const struct uui_keycapture *k = w;
    return uui_hit(k->x, k->y, k->w, k->h, cx, cy);
}

static int kc_press(void *w, int cx, int cy, unsigned mods) {
    (void)w; (void)cx; (void)cy; (void)mods;
    // ARMED ON THE RELEASE, NOT HERE. on_click fires on button-DOWN
    // despite its name, and a control that commits there can never be
    // cancelled (docs/gui-guidelines.md); this one takes the press only
    // to claim the grab.
    return 1;
}

static int kc_release(void *w, int cx, int cy) {
    struct uui_keycapture *k = w;
    if (!uui_hit(k->x, k->y, k->w, k->h, cx, cy)) return 0;
    if (k->armed) { uui_keycapture_cancel(k); return 1; }
    k->armed = 1;
    if (k->on_arm) k->on_arm(k->ctx, 1);
    return 1;
}

static int kc_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_keycapture *k = w;
    int was = k->hovered;
    k->hovered = uui_hit(k->x, k->y, k->w, k->h, cx, cy);
    return was != k->hovered;
}

static int kc_key(void *w, int key, unsigned mods) {
    struct uui_keycapture *k = w;
    if (!k->armed) {
        // Space or Enter arms it from the keyboard, which is how every
        // other focusable control here is activated.
        if (key == ' ' || key == '\n' || key == '\r') {
            k->armed = 1;
            if (k->on_arm) k->on_arm(k->ctx, 1);
            return 1;
        }
        return 0;
    }

    // ESC CANCELS AND BACKSPACE UNBINDS, which is GNOME's pair exactly.
    // Without a way to say "none" a shortcut could be changed but never
    // switched off.
    if (key == 0x1B) { uui_keycapture_cancel(k); return 1; }
    if (key == 0x08) {
        k->text[0] = '\0';
        disarm(k);
        if (k->on_done) k->on_done(k->ctx, "");
        return 1;
    }

    // A BARE MODIFIER IS NOT A COMBINATION, and it arrives as its own
    // key here -- so reaching for Ctrl before pressing T must not end
    // the capture. Swallowed, and the control keeps listening.
    if (key == KEY_SHIFT || key == KEY_CTRL || key == KEY_ALT ||
        key == KEY_ALTGR || key == KEY_SUPER)
        return 1;

    struct keycombo c = { .key = (uint8_t)key, .mods = (uint8_t)mods };
    char text[KEYCOMBO_TEXT_MAX];
    if (!keycombo_format(&c, text, sizeof text)) {
        // Nothing this grammar can spell -- keep listening rather than
        // recording something that will not parse back.
        return 1;
    }
    // **THE CONTROL CODE HAS TO BECOME A LETTER AGAIN.** Ctrl+T reaches
    // us as 0x14 (api/keyboard.h), which formats as an unprintable
    // character; the combination a person means is "Ctrl+T", so the
    // letter is recovered here -- the exact inverse of the fold
    // keycombo_matches() undoes on the way back in.
    if ((mods & KEY_MOD_CTRL) && key >= 1 && key <= 26) {
        c.key = (uint8_t)('A' + key - 1);
        if (!keycombo_format(&c, text, sizeof text)) return 1;
    }

    strlcpy(k->text, text, sizeof k->text);
    disarm(k);
    if (k->on_done) k->on_done(k->ctx, k->text);
    return 1;
}

static void kc_set_focused(void *w, int focused) {
    struct uui_keycapture *k = w;
    k->focused = focused;
    // LOSING THE FOCUS ENDS THE CAPTURE. The compositor drops the
    // inhibitor when the WINDOW loses focus (abi/win_proto.h); this is
    // the same rule one level down, so a control cannot sit armed behind
    // whatever was clicked instead.
    if (!focused) uui_keycapture_cancel(k);
}

static int kc_accepts_focus(const void *w) { (void)w; return 1; }

static void kc_describe(const void *w, const struct uui_describe *d) {
    const struct uui_keycapture *k = w;
    uui_describe_rect(d, "keycapture", k->x, k->y, k->w, k->h);
    uui_describe_str(d, "keycapture.value", k->armed ? PROMPT : k->text);
}

// EVERY SLOT FILLED AGAINST ui/uui_widget.h, not against whatever widget
// this was started from -- a missing one fails silently and at a
// distance (CLAUDE.md), and tools/check_widget_ops.py refuses the pairs
// that matter (draw needs natural_size and set_geometry; set_geometry
// needs bounds; press needs release; key needs accepts_focus).
const struct uui_widget_ops uui_keycapture_ops = {
    .natural_size  = kc_natural_size,
    .set_geometry  = kc_set_geometry,
    .bounds        = kc_bounds,
    .draw          = kc_draw,
    .hit           = kc_hit,
    .press         = kc_press,
    .release       = kc_release,
    .motion        = kc_motion,
    .key           = kc_key,
    .set_focused   = kc_set_focused,
    .accepts_focus = kc_accepts_focus,
    .describe      = kc_describe,
};
