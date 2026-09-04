// See brightness_popup.h. The mechanics are volume_popup.c's: one
// setting, a debounced write, a tray item, an overlay-table row.
#include "wm_internal.h"
#include "brightness_popup.h"
#include "volume_popup.h"
#include "wm_tray.h"
#include "start_menu.h"
#include "context_menu.h"
#include "calendar_popup.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // wm_setting_generation()
#include "lib/usetting.h"

#define BRIGHTNESS_SETTING "system.brightness"
#define BRIGHTNESS_STEP 5
#define BRIGHTNESS_COMMIT_MS 250

int brightness_open = 0;

static int g_tray_id = -1;
static int g_level = 100;
static int g_min = 5, g_max = 100;     // the registry's range, read once
static int g_pending;
static unsigned long long g_pending_at;
static char g_unavailable[SETTING_ABI_DESC_MAX];
static uint32_t g_seen_generation;
static int g_dragging;

#define HOVER_NONE   0
#define HOVER_SLIDER 1
static int g_hover = HOVER_NONE;

// --- the setting behind it --------------------------------------------

// INFO rather than GET: the one op that carries the range and the
// `unavailable` sentence beside the value.
static void reload(void) {
    struct setting_msg m;
    if (usetting_find(BRIGHTNESS_SETTING, &m) < 0) return;
    uint32_t v;
    if (k_parse_u32(m.value, &v) && v <= 100) g_level = (int)v;
    if (m.imax > m.imin) { g_min = m.imin; g_max = m.imax; }
    k_strlcpy(g_unavailable, m.unavailable, sizeof g_unavailable);
}

const char *brightness_unavailable_text(void) { return g_unavailable; }

// --- the tray item ----------------------------------------------------

void brightness_tray_init(void) {
    reload();
    g_tray_id = tray_register_icon("tray-brightness");
}

// --- geometry ---------------------------------------------------------

void brightness_geometry(struct brightness_geom *g) {
    for (unsigned i = 0; i < sizeof *g; i++) ((uint8_t *)g)[i] = 0;

    int ch = ugfx_char_h();
    int pad = ch / 2 + 2;
    int row_h = ch + 8;
    int available = g_unavailable[0] == 0;
    // Wide enough for the sentence when there is one, so it is never
    // clipped; the slider alone wants the volume flyout's width.
    int w = ugfx_char_w() * 22 + pad * 2;
    if (!available) {
        int tw = ugfx_text_width(g_unavailable) + pad * 2;
        if (tw > w) w = tw;
    }
    int h = pad + row_h + pad;
    if (!available) h += ch + pad / 2;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    int x = tx + tw - w;
    if (x + w > screen_w - 8) x = screen_w - 8 - w;
    if (x < 4) x = 4;
    int y = screen_h - taskbar_h - h - 4;
    if (y < 4) y = 4;

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->icon_x = x + pad;
    g->icon_y = y + pad;
    g->icon_w = row_h;
    g->icon_h = row_h;
    g->slider_x = g->icon_x + g->icon_w + pad;
    g->slider_y = y + pad + (row_h - ch) / 2;
    g->slider_h = ch;
    g->slider_w = x + w - pad - ugfx_text_width("100%") - pad - g->slider_x;
    g->level = g_level;
    g->available = available;
}

// --- state ------------------------------------------------------------

void brightness_damage(void) {
    struct brightness_geom g;
    brightness_geometry(&g);
    wm_damage_rect(g.x, g.y, g.w, g.h);
    redraw_pending = 1;
}

static int hover_at(const struct brightness_geom *g, int mx, int my) {
    if (!g->available) return HOVER_NONE;
    if (uui_hit(g->slider_x - 4, g->y, g->slider_w + 8, g->icon_h + 8, mx, my))
        return HOVER_SLIDER;
    return HOVER_NONE;
}

int brightness_hover_at(int mx, int my) {
    if (!brightness_open) { g_hover = HOVER_NONE; return 0; }
    struct brightness_geom g;
    brightness_geometry(&g);
    g_hover = hover_at(&g, mx, my);
    return g_hover;
}

void brightness_open_now(void) {
    if (start_menu_open) { start_menu_open = 0; start_menu_damage(); }
    calendar_close();
    context_menu_close();
    volume_close();
    reload();
    brightness_open = 1;
    g_hover = HOVER_NONE;
    brightness_damage();
}

void brightness_close(void) {
    if (!brightness_open) return;
    brightness_damage();   // before the flag drops -- see volume_close()
    brightness_open = 0;
    g_dragging = 0;
    g_hover = HOVER_NONE;
}

static void set_level(int pct, int commit_now) {
    if (g_unavailable[0]) return;   // the registry would refuse it anyway
    if (pct < g_min) pct = g_min;
    if (pct > g_max) pct = g_max;
    if (pct == g_level && !commit_now) return;
    g_level = pct;
    g_pending = 1;
    g_pending_at = commit_now ? 0 : sys_monotonic_ns();
    if (brightness_open) brightness_damage();
}

void brightness_poll_config(void) {
    if (g_pending) {
        unsigned long long now = sys_monotonic_ns();
        if (g_pending_at == 0 ||
            now - g_pending_at >= (unsigned long long)BRIGHTNESS_COMMIT_MS * 1000000ull) {
            usetting_set_int(BRIGHTNESS_SETTING, g_level);
            g_pending = 0;
            g_seen_generation = wm_setting_generation();
        }
        return;
    }
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return;
    g_seen_generation = gen;
    reload();
    if (brightness_open) redraw_pending = 1;
}

// --- input ------------------------------------------------------------

static int level_from_x(const struct brightness_geom *g, int mx) {
    if (g->slider_w <= 0) return g_level;
    int rel = mx - g->slider_x;
    if (rel < 0) rel = 0;
    if (rel > g->slider_w) rel = g->slider_w;
    return rel * 100 / g->slider_w;
}

int brightness_handle_click(int mx, int my) {
    struct brightness_geom g;
    brightness_geometry(&g);

    if (!brightness_open) {
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            brightness_open_now();
            return 1;
        }
        return 0;
    }
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        brightness_close();
        return my < screen_h - taskbar_h;   // a taskbar click falls through
    }
    if (g.available &&
        uui_hit(g.slider_x - 4, g.y, g.slider_w + 8, g.icon_h + 8, mx, my)) {
        g_dragging = 1;
        set_level(level_from_x(&g, mx), 0);
    }
    return 1;
}

void brightness_update_press(int mx, int my, uint8_t buttons) {
    (void)my;
    if (!g_dragging) return;
    if (!(buttons & 0x1)) {
        g_dragging = 0;
        if (g_pending) g_pending_at = 0;   // release is the moment meant
        return;
    }
    struct brightness_geom g;
    brightness_geometry(&g);
    int want = level_from_x(&g, mx);
    if (want != g_level) set_level(want, 0);
}

int brightness_handle_wheel(int mx, int my, int notches) {
    if (!notches) return 0;
    struct brightness_geom g;
    brightness_geometry(&g);
    int over = uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my) ||
               (brightness_open && uui_hit(g.x, g.y, g.w, g.h, mx, my));
    if (!over) return 0;
    set_level(g_level + notches * BRIGHTNESS_STEP, 0);
    return 1;   // consumed even when unavailable: the notch was aimed here
}

// --- drawing ----------------------------------------------------------

void brightness_draw(int mx, int my) {
    if (!brightness_open) return;
    (void)mx; (void)my;   // the tracked hover is the one that damaged

    struct brightness_geom g;
    brightness_geometry(&g);
    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    uint32_t accent = g.available ? UTHEME_ACCENT : border;

    ugfx_fill_rect(wm_surface(), g.x, g.y, g.w, g.h, bg);

    const struct uimg *ico = icon_get("tray-brightness", g.icon_w - 6);
    if (ico)
        ugfx_blit_alpha(wm_surface(), g.icon_x + (g.icon_w - ico->w) / 2,
                        g.icon_y + (g.icon_h - ico->h) / 2,
                        ico->w, ico->h, ico->px, ico->w);

    int track_y = g.slider_y + g.slider_h / 2 - 2;
    ugfx_fill_rect(wm_surface(), g.slider_x, track_y, g.slider_w, 4, border);
    int filled = g.slider_w * g.level / 100;
    ugfx_fill_rect(wm_surface(), g.slider_x, track_y, filled, 4, accent);
    ugfx_fill_rect(wm_surface(), g.slider_x + filled - 2, g.slider_y - 2,
                   5, g.slider_h + 4, accent);

    char pct[8];
    k_snprintf(pct, sizeof pct, "%d%%", g.level);
    ugfx_draw_string_clipped(wm_surface(), g.x + g.w - ugfx_text_width("100%") - 8,
                             g.slider_y, ugfx_text_width("100%") + 4, pct,
                             g.available ? fg : border, bg);

    if (!g.available) {
        int ty = g.icon_y + g.icon_h + (ugfx_char_h() / 2 + 2) / 2;
        ugfx_draw_string_clipped(wm_surface(), g.icon_x, ty, g.w - (g.icon_x - g.x) * 2,
                                 g_unavailable, border, bg);
    }
    ugfx_draw_rect(wm_surface(), g.x, g.y, g.w, g.h, border);
}
