// The tray flyout card -- see wm_flyout.h.
#include "wm_internal.h"
#include "wm_flyout.h"
#include "wm_glass.h"
#include "wm_shadow.h"
#include "lib/icon_cache.h"
#include "ui/uui.h"
#include "ui/uui_popup.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"

void wm_flyout_metrics(struct wm_flyout_metrics *m) {
    int ch = ugfx_char_h();
    m->pad = ch;
    m->vpad = ch / 2 + 2;
    m->radius = uui_popup_radius();
    m->badge = 2 * ch + 6;
    m->hero_h = m->badge + 2 * m->vpad;
    m->cap_h = ch + 4;
    m->row_h = ch + 6;
    m->btn_h = ch + 12;
    m->foot_h = m->btn_h + 12;
    m->sw_h = ch + 3;
    m->sw_w = 2 * m->sw_h;
}

uint32_t wm_flyout_ground(void) { return uui_popup_bg(); }
// AGAINST WHAT IS THERE, always: the card's ground, its footer band and
// glass are three different colours under one label style, and a flat
// guess blends every glyph edge toward the wrong one.
uint32_t wm_flyout_ink(void) { return UGFX_TRANSPARENT; }
uint32_t wm_flyout_dim(void) { return ugfx_blend(UTHEME_TEXT, uui_popup_bg(), 110); }

static uint32_t foot_bg(void) { return ugfx_blend(uui_popup_bg(), UTHEME_CHROME, 128); }
static uint32_t rule_c(void) { return ugfx_blend(uui_popup_border(), uui_popup_bg(), 96); }

void wm_flyout_card(int x, int y, int w, int h, int foot_h) {
    struct ugfx_surface *s = wm_surface();
    int r = uui_popup_radius();
    int glass = wm_glass_on(WM_GLASS_MENU);
    (glass ? wm_shadow_draw_hollow : wm_shadow_draw)(x, y, w, h, r, WM_SHADOW_POPUP);
    wm_glass_paint(WM_GLASS_MENU, x, y, w, h, r, wm_flyout_ground());
    if (foot_h > 0) {
        // The band follows the card's lower arcs: the card's own rounded
        // rect, clipped to the band. Over glass it is a wash, not a fill.
        struct ugfx_clip c;
        ugfx_clip_save(s, &c);
        ugfx_clip_intersect(s, x, y + h - foot_h, w, foot_h);
        uui_glass_round_rect(s, x, y, w, h, r, foot_bg(), glass ? 150 : 255, 0);
        ugfx_clip_restore(s, &c);
        ugfx_fill_rect(s, x + 1, y + h - foot_h, w - 2, 1, rule_c());
    }
    uui_glass_round_rect(s, x, y, w, h, r, uui_popup_border(), 0, 255);
}

void wm_flyout_hero(int x, int y, int w, uint32_t badge, const char *icon,
                    const char *title, const char *sub) {
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    struct ugfx_surface *s = wm_surface();
    int bx = x + m.pad, by = y + m.vpad;
    uui_fill_round_rect(s, bx, by, m.badge, m.badge, UUI_CAPSULE, badge);
    const struct uimg *ico = icon ? icon_get(icon, m.badge * 11 / 20) : 0;
    if (ico)
        ugfx_blit_tinted(s, bx + (m.badge - ico->w) / 2, by + (m.badge - ico->h) / 2,
                         ico->w, ico->h, ico->px, ico->w, UTHEME_WHITE);

    int ch = ugfx_char_h();
    int tx = bx + m.badge + m.pad * 3 / 4;
    int ty = by + (m.badge - 2 * ch - 2) / 2;
    int tw = x + w - tx;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, tx, ty, tw, title, UTHEME_TEXT, wm_flyout_ink());
    ugfx_set_font(was);
    if (sub && sub[0])
        ugfx_draw_string_elided(s, tx, ty + ch + 2, tw, sub, wm_flyout_dim(), wm_flyout_ink());
}

void wm_flyout_rule(int x, int y, int w) {
    ugfx_fill_rect(wm_surface(), x + 1, y, w - 2, 1, rule_c());
}

void wm_flyout_caption(int x, int y, int w, const char *text) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(wm_surface(), x, y, w, text, wm_flyout_dim(), wm_flyout_ink());
    ugfx_set_font(was);
}

void wm_flyout_kv(int x, int y, int w, int key_w, const char *key, const char *val) {
    ugfx_draw_string_clipped(wm_surface(), x, y, key_w - 4, key, wm_flyout_dim(), wm_flyout_ink());
    ugfx_draw_string_elided(wm_surface(), x + key_w, y, w - key_w, val, UTHEME_TEXT, wm_flyout_ink());
}

int wm_flyout_button_w(const char *label, const char *icon) {
    int ch = ugfx_char_h();
    return ugfx_text_width(label) + 2 * (ch * 3 / 4) + (icon ? ch + ch / 2 : 0);
}

int wm_flyout_button(int x, int y, const char *label, const char *icon,
                     int outlined, int hot, int off) {
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    struct ugfx_surface *s = wm_surface();
    int ch = ugfx_char_h();
    int w = wm_flyout_button_w(label, icon);
    uint32_t face = outlined ? UTHEME_WHITE : foot_bg();
    if (off) hot = 0;
    if (hot) face = uui_state_bg(face, UUI_STATE_HOVER);
    uint32_t ink = off ? ugfx_blend(UTHEME_TEXT, face, 150) : UTHEME_TEXT;
    if (outlined) {
        uui_fill_round_rect(s, x, y, w, m.btn_h, 5, UTHEME_OUTLINE);
        uui_fill_round_rect(s, x + 1, y + 1, w - 2, m.btn_h - 2, 4, face);
    } else if (hot) {
        uui_fill_round_rect(s, x, y, w, m.btn_h, 5, face);
    }
    uint32_t text_bg = (outlined || hot) ? face : wm_flyout_ink();
    int tx = x + ch * 3 / 4;
    if (icon) {
        const struct uimg *ico = icon_get(icon, ch);
        if (ico)
            ugfx_blit_tinted(s, tx, y + (m.btn_h - ico->h) / 2, ico->w, ico->h, ico->px,
                             ico->w, ugfx_blend(ink, face, 40));
        tx += ch + ch / 2;
    }
    ugfx_draw_string_clipped(s, tx, y + (m.btn_h - ch) / 2, x + w - tx, label,
                             ink, text_bg);
    return w;
}

void wm_flyout_switch(int x, int y, int on, int hot) {
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    struct ugfx_surface *s = wm_surface();
    int k = m.sw_h - 6;   // the knob
    if (on) {
        uint32_t c = hot ? uui_state_bg(UTHEME_ACCENT, UUI_STATE_HOVER) : UTHEME_ACCENT;
        uui_fill_round_rect(s, x, y, m.sw_w, m.sw_h, UUI_CAPSULE, c);
        uui_fill_round_rect(s, x + m.sw_w - 3 - k, y + 3, k, k, UUI_CAPSULE, UTHEME_WHITE);
    } else {
        uint32_t c = hot ? uui_state_bg(UTHEME_WHITE, UUI_STATE_HOVER) : UTHEME_WHITE;
        uui_fill_round_rect(s, x, y, m.sw_w, m.sw_h, UUI_CAPSULE, ugfx_rgb(130, 130, 140));
        uui_fill_round_rect(s, x + 1, y + 1, m.sw_w - 2, m.sw_h - 2, UUI_CAPSULE, c);
        uui_fill_round_rect(s, x + 3, y + 3, k, k, UUI_CAPSULE, UTHEME_TEXT);
    }
}

void wm_flyout_icon_button(int x, int y, int size, const char *icon, int hot) {
    struct ugfx_surface *s = wm_surface();
    if (hot) uui_fill_round_rect(s, x, y, size, size, 5, uui_state_bg(foot_bg(), UUI_STATE_HOVER));
    int ch = ugfx_char_h();
    const struct uimg *ico = icon_get(icon, ch);
    if (ico)
        ugfx_blit_tinted(s, x + (size - ico->w) / 2, y + (size - ico->h) / 2, ico->w, ico->h,
                         ico->px, ico->w, ugfx_blend(UTHEME_TEXT, foot_bg(), 40));
}

void wm_flyout_radio_row(int x, int y, int w, int h, int selected, int hot,
                         const char *label, const char *note) {
    struct ugfx_surface *s = wm_surface();
    int ch = ugfx_char_h();
    uint32_t bg = wm_flyout_ink();
    if (selected) {
        uui_fill_round_rect(s, x, y, w, h, 5, ugfx_blend(UTHEME_SELECTION, UTHEME_ACCENT, 110));
        uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, 4, UTHEME_SELECTION);
        bg = UTHEME_SELECTION;
    } else if (hot) {
        uui_fill_round_rect(s, x, y, w, h, 5, uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER));
        bg = uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER);
    }
    int d = ch * 3 / 4 + 2;                 // the radio's diameter
    int rx = x + ch / 2, ry = y + (h - d) / 2;
    uui_fill_round_rect(s, rx, ry, d, d, UUI_CAPSULE, selected ? UTHEME_ACCENT : ugfx_rgb(130, 130, 140));
    uui_fill_round_rect(s, rx + 2, ry + 2, d - 4, d - 4, UUI_CAPSULE, selected ? UTHEME_SELECTION : UTHEME_WHITE);
    if (selected) uui_fill_round_rect(s, rx + d / 4 + 1, ry + d / 4 + 1, d - 2 * (d / 4) - 2,
                                      d - 2 * (d / 4) - 2, UUI_CAPSULE, UTHEME_ACCENT);
    int tx = rx + d + ch / 2;
    int nw = note && note[0] ? ugfx_text_width(note) : 0;
    int avail = x + w - tx - (nw ? nw + ch : ch / 2);
    ugfx_draw_string_elided(s, tx, y + (h - ch) / 2, avail, label, UTHEME_TEXT, bg);
    if (nw)
        ugfx_draw_string_clipped(s, x + w - ch / 2 - nw, y + (h - ch) / 2, nw + 2, note,
                                 wm_flyout_dim(), bg);
}
