// See brightness_popup.h. Everything but the `unavailable` sentence is
// tray_slider_popup.c's.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "brightness_popup.h"
#include "tray_slider_popup.h"
#include "wm_tray.h"     // tray_want_shown, tray_set_hidden
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"

#define BRIGHTNESS_SETTING "system.brightness"
#define BRIGHTNESS_STEP 5

int brightness_open = 0;

static struct tray_slider_popup g_popup;
static int g_hover;

const char *brightness_unavailable_text(void) { return g_popup.unavailable; }

int brightness_tray_hidden(void) { return tray_is_hidden(g_popup.tray_id); }

// `auto` shows the sun only where there is a backlight to move, which
// is what Windows 11 and Plasma both do. The item is REGISTERED either
// way and hidden in its slot, so `always` -- which is how a GUI tool
// keeps this flyout covered under QEMU -- costs no re-registration and
// cannot reorder the strip. See tray_config.c.
static void apply_visibility(void) {
    int has_backlight = g_popup.unavailable[0] == 0;
    tray_set_hidden(g_popup.tray_id, !tray_want_shown("brightness", has_backlight));
}

void brightness_tray_init(void) {
    g_popup.name = "brightness";
    g_popup.setting = BRIGHTNESS_SETTING;
    g_popup.step = BRIGHTNESS_STEP;
    g_popup.damage = brightness_damage;
    // ON THE GENERATION, NEVER PER FRAME. tray_want_shown() is a
    // sys_setting() GET, and the kernel getter behind it reads
    // /etc/desktop.conf -- so calling it every frame is a whole-file
    // disk read at frame rate, which is the cost the WM's poll
    // convention exists to avoid (wm.c's desktop-entry note).
    // tray_slider_poll() fires this only when some setting moved.
    g_popup.on_reload = apply_visibility;
    tray_slider_init(&g_popup, "tray-brightness");
    apply_visibility();
}

// --- geometry ---------------------------------------------------------

static void geometry(struct tray_slider_geom *s, struct brightness_geom *g) {
    int ch = ugfx_char_h();
    int pad = ch / 2 + 2;
    int available = g_popup.unavailable[0] == 0;
    // Wide enough for the sentence when there is one, so it is never
    // clipped; the slider alone wants the shared width.
    int want_w = available ? 0 : ugfx_text_width(g_popup.unavailable) + pad * 2;
    int extra_h = available ? 0 : ch + pad / 2;
    tray_slider_geometry(&g_popup, want_w, extra_h, s);
    if (!g) return;

    k_memset(g, 0, sizeof *g);
    g->x = s->x; g->y = s->y; g->w = s->w; g->h = s->h;
    g->tray_x = s->tray_x; g->tray_y = s->tray_y; g->tray_w = s->tray_w; g->tray_h = s->tray_h;
    g->icon_x = s->icon_x; g->icon_y = s->icon_y; g->icon_w = s->icon_w; g->icon_h = s->icon_h;
    g->slider_x = s->slider_x; g->slider_y = s->slider_y;
    g->slider_w = s->slider_w; g->slider_h = s->slider_h;
    g->level = g_popup.level;
    g->available = available;
}

void brightness_geometry(struct brightness_geom *g) {
    struct tray_slider_geom s;
    geometry(&s, g);
}

static void slider_geom(struct tray_slider_geom *s) { geometry(s, 0); }

// --- state ------------------------------------------------------------

int brightness_rect(int *x, int *y, int *w, int *h) {
    struct tray_slider_geom s;
    slider_geom(&s);
    *x = s.x; *y = s.y; *w = s.w; *h = s.h;
    return 1;
}

void brightness_damage(void) { wm_overlay_damage("brightness"); }

int brightness_hover_at(int mx, int my) {
    struct tray_slider_geom s;
    slider_geom(&s);
    g_hover = tray_slider_hover_at(&g_popup, &s, mx, my);
    // The sun is a picture, not a control: no hover for it.
    if (g_hover == TRAY_SLIDER_HOVER_ICON) g_hover = TRAY_SLIDER_HOVER_NONE;
    return g_hover;
}

void brightness_open_now(void) {
    tray_slider_open(&g_popup);
    brightness_open = g_popup.open;
    g_hover = TRAY_SLIDER_HOVER_NONE;
}

void brightness_close(void) {
    tray_slider_close(&g_popup);
    brightness_open = g_popup.open;
    g_hover = TRAY_SLIDER_HOVER_NONE;
}

void brightness_poll_config(void) {
    tray_slider_poll(&g_popup);
}

// --- input ------------------------------------------------------------

int brightness_handle_click(int mx, int my) {
    struct tray_slider_geom s;
    slider_geom(&s);
    enum tray_slider_click what = tray_slider_click(&g_popup, &s, mx, my);
    brightness_open = g_popup.open;
    if (what == TRAY_SLIDER_CLICK_NONE) return 0;
    if (what == TRAY_SLIDER_CLICK_DISMISSED)
        return my < screen_h - taskbar_h;   // a taskbar click falls through
    return 1;
}

void brightness_update_press(int mx, int my, uint8_t buttons) {
    (void)my;
    if (!g_popup.scale.dragging) return;
    struct tray_slider_geom s;
    slider_geom(&s);   // places the scale before the drag reads it
    tray_slider_update_press(&g_popup, mx, buttons);
}

int brightness_handle_wheel(int mx, int my, int notches) {
    struct tray_slider_geom s;
    slider_geom(&s);
    return tray_slider_wheel(&g_popup, &s, mx, my, notches);
}

// --- drawing ----------------------------------------------------------

void brightness_draw(int mx, int my) {
    if (!brightness_open) return;
    (void)mx; (void)my;   // the tracked hover is the one that damaged

    struct tray_slider_geom s;
    struct brightness_geom g;
    geometry(&s, &g);

    tray_slider_draw(&g_popup, &s, "tray-brightness", 0);

    if (!g.available) {
        int ty = s.below_y + s.pad / 2;
        ugfx_draw_string_clipped(wm_surface(), g.icon_x, ty, g.w - (g.icon_x - g.x) * 2,
                                 g_popup.unavailable, UTHEME_BORDER, UTHEME_PANEL_BG);
    }
}
