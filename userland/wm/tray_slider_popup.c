// See tray_slider_popup.h for what this is and why the write is
// debounced.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "tray_slider_popup.h"
#include "wm_shadow.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_log.h"   // the anchor-lookup probe below
#include "ui/uui.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // wm_setting_generation()
#include "lib/usetting.h"

// INFO rather than GET: the one op that carries the range and the
// `unavailable` sentence beside the value.
static void reload(struct tray_slider_popup *p) {
    struct setting_msg m;
    if (usetting_find(p->setting, &m) < 0) return;
    if (m.imax > m.imin) { p->min = m.imin; p->max = m.imax; }
    uint32_t v;
    if (k_parse_u32(m.value, &v) && (int)v >= p->min && (int)v <= p->max)
        p->level = (int)v;
    k_strlcpy(p->unavailable, m.unavailable, sizeof p->unavailable);
    uui_scale_set_range(&p->scale, p->min, p->max);
    uui_scale_set_value(&p->scale, p->level);
    p->scale.disabled = p->unavailable[0] != 0;
    if (p->on_level) p->on_level();
}

void tray_slider_init(struct tray_slider_popup *p, const char *icon) {
    p->open = 0;
    p->tray_id = -1;   // not registered yet: on_level must not touch the tray
    p->min = 0; p->max = 100; p->level = 100;
    uui_scale_init(&p->scale, 0, 100, 100);
    p->scale.step = p->step;
    reload(p);
    p->tray_id = tray_register_icon(icon);
}

// --- geometry ---------------------------------------------------------

void tray_slider_geometry(struct tray_slider_popup *p, int want_w, int extra_h,
                          int foot_h, struct tray_slider_geom *g) {
    k_memset(g, 0, sizeof *g);

    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int ch = ugfx_char_h();
    int pad = m.pad;
    int row_h = ch + 16;
    int caption_w = ugfx_text_width("100%");

    int w = ch * 20;
    if (want_w > w) w = want_w;
    int h = m.hero_h + 1 + m.vpad + row_h + m.vpad + extra_h + foot_h;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(p->tray_id, &tx, &ty, &tw, &th)) {
        // THE PANEL IS ANCHORED TO ITS TRAY ITEM, so a lookup that
        // fails moves the whole panel -- which between two render
        // passes reads as the panel vanishing from where it was.
        if (wm_damage_verify_enabled())
            wm_logf("wm: tray_item_rect(%d) FAILED -- %s falls back\n",
                    p->tray_id, p->name ? p->name : "?");
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->hero_h = m.hero_h;
    g->foot_h = foot_h;
    g->foot_y = y + h - foot_h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->pad = pad; g->row_h = row_h;
    int row_y = y + m.hero_h + 1 + m.vpad;
    g->icon_x = x + pad - 6;
    g->icon_y = row_y;
    g->icon_w = row_h;
    g->icon_h = row_h;
    g->slider_x = g->icon_x + g->icon_w + pad / 2;
    g->slider_y = row_y + (row_h - ch) / 2;
    g->slider_h = ch;
    // The caption sits at the right end, so the track stops short of it.
    g->slider_w = x + w - pad - caption_w - pad / 2 - g->slider_x;
    g->below_y = row_y + row_h + m.vpad;

    uui_scale_set_geometry(&p->scale, g->slider_x, g->slider_y, g->slider_w, g->slider_h);
}

// The band a press or a hover on the track accepts: taller than the
// scale, so the thumb is not the only target. Hover and click use the
// SAME band -- a highlight somewhere a click would not land is worse
// than none.
static int over_track(const struct tray_slider_geom *g, int mx, int my) {
    return uui_hit(g->slider_x - 4, g->icon_y, g->slider_w + 8, g->row_h, mx, my);
}

// --- state ------------------------------------------------------------

void tray_slider_damage(const struct tray_slider_geom *g) {
    wm_damage_window_rect(g->x, g->y, g->w, g->h);   // plus its shadow (wm_shadow.h)
    redraw_pending = 1;
}

static void damage(struct tray_slider_popup *p) {
    if (p->damage) p->damage();
    else redraw_pending = 1;
}

void tray_slider_open(struct tray_slider_popup *p) {
    wm_overlay_close_others(p->name);
    reload(p);
    if (p->on_reload) p->on_reload();
    p->open = 1;
    p->scale.hovered = 0;
    damage(p);
}

void tray_slider_close(struct tray_slider_popup *p) {
    if (!p->open) return;
    p->open = 0;   // the core damages where it was and asks for the frame (wm_overlay.h)
    p->scale.dragging = 0;
    p->scale.hovered = 0;
}

void tray_slider_set_level(struct tray_slider_popup *p, int level, int commit_now) {
    if (p->unavailable[0]) return;   // the registry would refuse it anyway
    if (level < p->min) level = p->min;
    if (level > p->max) level = p->max;
    if (level == p->level && !commit_now) return;
    p->level = level;
    uui_scale_set_value(&p->scale, level);
    p->pending = 1;
    p->pending_at = commit_now ? 0 : sys_monotonic_ns();
    if (p->on_level) p->on_level();
    if (p->open) damage(p);
    else redraw_pending = 1;   // the tray item may show the level
}

void tray_slider_poll(struct tray_slider_popup *p) {
    int refused = 0;
    if (p->pending) {
        unsigned long long now = sys_monotonic_ns();
        if (p->pending_at != 0 &&
            now - p->pending_at < (unsigned long long)TRAY_SLIDER_COMMIT_MS * 1000000ull)
            return;
        p->pending = 0;
        refused = usetting_set_int(p->setting, p->level) <= 0;
        // Our own write bumps the generation; adopting it here stops the
        // reload below from re-reading what we just wrote. A REFUSED
        // write goes on to that reload, so the slider shows what the
        // registry holds rather than the value it would not take.
        p->seen_generation = wm_setting_generation();
        if (!refused) return;
    }
    uint32_t gen = wm_setting_generation();
    if (gen == p->seen_generation && !refused) return;
    p->seen_generation = gen;
    // Something else changed a setting -- System Settings, `config
    // set`, a card appearing -- or ours was refused. Re-read, and let
    // the owner re-read too.
    reload(p);
    if (p->on_reload) p->on_reload();
    // **DAMAGE, NOT JUST REPAINT: on_reload CAN RESIZE THE PANEL.** Its
    // rows and their label widths both come from what it re-read, so a
    // card appearing or going away moves the top and the left edge --
    // and a plain repaint leaves the strip the LARGER panel covered
    // sitting on the desktop. damage() covers the last drawn rect as
    // well as the new one (wm_overlay.h); open and close already went
    // through it, and this path did not.
    if (p->open) damage(p);
    else redraw_pending = 1;
}

// --- input ------------------------------------------------------------

int tray_slider_hover_at(struct tray_slider_popup *p, const struct tray_slider_geom *g,
                         int mx, int my) {
    if (!p->open) { p->scale.hovered = 0; return TRAY_SLIDER_HOVER_NONE; }
    if (!uui_hit(g->x, g->y, g->w, g->h, mx, my)) {
        p->scale.hovered = 0;
        return TRAY_SLIDER_HOVER_NONE;
    }
    if (uui_hit(g->icon_x, g->icon_y, g->icon_w, g->icon_h, mx, my)) {
        p->scale.hovered = 0;
        return TRAY_SLIDER_HOVER_ICON;
    }
    int on = !p->scale.disabled && over_track(g, mx, my);
    p->scale.hovered = on;
    return on ? TRAY_SLIDER_HOVER_TRACK : TRAY_SLIDER_HOVER_NONE;
}

enum tray_slider_click tray_slider_click(struct tray_slider_popup *p,
                                         const struct tray_slider_geom *g,
                                         int mx, int my) {
    if (!p->open) {
        if (!uui_hit(g->tray_x, g->tray_y, g->tray_w, g->tray_h, mx, my))
            return TRAY_SLIDER_CLICK_NONE;
        tray_slider_open(p);
        return TRAY_SLIDER_CLICK_OPENED;
    }
    if (!uui_hit(g->x, g->y, g->w, g->h, mx, my)) {
        tray_slider_close(p);
        return TRAY_SLIDER_CLICK_DISMISSED;
    }
    if (uui_hit(g->icon_x, g->icon_y, g->icon_w, g->icon_h, mx, my))
        return TRAY_SLIDER_CLICK_ICON;
    if (!p->scale.disabled && over_track(g, mx, my)) {
        // A click anywhere on the track JUMPS there, as on every real
        // scale, and the drag begins from it.
        p->scale.dragging = 1;
        tray_slider_set_level(p, (int)uui_scale_value_at(&p->scale, mx), 0);
        return TRAY_SLIDER_CLICK_TRACK;
    }
    return TRAY_SLIDER_CLICK_INSIDE;
}

void tray_slider_update_press(struct tray_slider_popup *p, int mx, uint8_t buttons) {
    if (!p->scale.dragging) return;
    if (!(buttons & 0x1)) {
        p->scale.dragging = 0;
        // Releasing the slider is the moment the user means; waiting
        // out the debounce would be a quarter second of the old level.
        if (p->pending) p->pending_at = 0;
        return;
    }
    tray_slider_set_level(p, (int)uui_scale_value_at(&p->scale, mx), 0);
}

int tray_slider_wheel(struct tray_slider_popup *p, const struct tray_slider_geom *g,
                      int mx, int my, int notches) {
    if (!notches) return 0;
    int over = uui_hit(g->tray_x, g->tray_y, g->tray_w, g->tray_h, mx, my) ||
               (p->open && uui_hit(g->x, g->y, g->w, g->h, mx, my));
    if (!over) return 0;
    tray_slider_set_level(p, p->level + notches * p->step, 0);
    return 1;   // consumed even when unavailable: the notch was aimed here
}

// --- drawing ----------------------------------------------------------

void tray_slider_draw(const struct tray_slider_popup *p, const struct tray_slider_geom *g,
                      const char *icon, int icon_hot) {
    int available = p->unavailable[0] == 0;

    wm_flyout_card(g->x, g->y, g->w, g->h, g->foot_h);
    char title[64] = "", sub[96] = "";
    if (p->hero) p->hero(title, sizeof title, sub, sizeof sub);
    wm_flyout_hero(g->x, g->y, g->w - g->pad, p->hero_badge ? p->hero_badge : UTHEME_ACCENT,
                   p->hero_icon ? p->hero_icon : icon, title, sub);
    wm_flyout_rule(g->x, g->y + g->hero_h, g->w);

    // Derived from the card's own colour, never hand-picked: on this
    // near-white theme "hover" has to DARKEN (docs/gui-guidelines.md).
    if (icon_hot)
        uui_fill_round_rect(wm_surface(), g->icon_x, g->icon_y, g->icon_w, g->icon_h, 6,
                            uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER));
    const struct uimg *ico = icon_get(icon, g->icon_w - 14);
    if (ico)
        ugfx_blit_tinted(wm_surface(), g->icon_x + (g->icon_w - ico->w) / 2,
                         g->icon_y + (g->icon_h - ico->h) / 2,
                         ico->w, ico->h, ico->px, ico->w, ugfx_blend(UTHEME_TEXT, wm_flyout_ground(), 40));

    uui_scale_draw(wm_surface(), &p->scale);

    char pct[8];
    k_snprintf(pct, sizeof pct, "%d%%", p->level);
    int caption_w = ugfx_text_width("100%");
    ugfx_draw_string_clipped(wm_surface(), g->x + g->w - g->pad - caption_w,
                             g->slider_y, caption_w + 4, pct,
                             available ? UTHEME_TEXT : wm_flyout_dim(), wm_flyout_ink());
}
