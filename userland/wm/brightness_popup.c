// See brightness_popup.h. Everything but the `unavailable` sentence is
// tray_slider_popup.c's.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "brightness_popup.h"
#include "tray_slider_popup.h"
#include "wm_tray.h"     // tray_want_shown, tray_set_hidden
#include "wm_flyout.h"
#include "lib/usetting.h"
#include "rt/sys.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"

#define BRIGHTNESS_SETTING "system.brightness"
#define BRIGHTNESS_STEP 5

int brightness_open = 0;

#define SCALING_SETTING "system.scaling"
#define SEG_MAX 4

static struct tray_slider_popup g_popup;
static int g_hover;
#define HOVER_SEG(i)  (TRAY_SLIDER_HOVER_OWNER + (i))
#define HOVER_BUTTON  (TRAY_SLIDER_HOVER_OWNER + SEG_MAX)

// What the header says, and the scaling choices: read on a settings
// change and on opening, never per frame (a query and a registry walk).
static char g_title[48], g_sub[64];
static char g_seg_value[SEG_MAX][SETTING_ABI_VALUE_MAX];
static char g_seg_label[SEG_MAX][SETTING_ABI_LABEL_MAX];
static int g_seg_count, g_seg_selected;

static void reload_display(void) {
    struct query_display d;
    k_strlcpy(g_title, "Built-in display", sizeof g_title);
    g_sub[0] = 0;
    if (sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d) >= (int)sizeof d) {
        if (d.panel[0]) k_strlcpy(g_title, d.panel, sizeof g_title);
        if (d.refresh_mhz)
            k_snprintf(g_sub, sizeof g_sub, "%llu x %llu, %llu Hz",
                       (unsigned long long)d.width, (unsigned long long)d.height,
                       (unsigned long long)((d.refresh_mhz + 500) / 1000));
        else
            k_snprintf(g_sub, sizeof g_sub, "%llu x %llu",
                       (unsigned long long)d.width, (unsigned long long)d.height);
    }

    // SCALING only where the registry offers it as something to change.
    g_seg_count = 0;
    struct setting_msg m;
    int index = usetting_find(SCALING_SETTING, &m);
    if (index < 0 || m.unavailable[0]) return;
    char current[SETTING_ABI_VALUE_MAX] = "";
    usetting_get(SCALING_SETTING, current, sizeof current);
    for (int c = 0; c < SEG_MAX; c++) {
        k_memset(&m, 0, sizeof m);
        m.op = SETTING_OP_CHOICE;
        m.index = index;
        m.choice = c;
        if (usetting_dispatch(&m) != 0) break;
        k_strlcpy(g_seg_value[c], m.value, sizeof g_seg_value[c]);
        k_strlcpy(g_seg_label[c], m.label, sizeof g_seg_label[c]);
        if (!k_strcmp(m.value, current)) g_seg_selected = c;
        g_seg_count = c + 1;
    }
}

static void hero(char *title, unsigned tcap, char *sub, unsigned scap) {
    k_strlcpy(title, g_title, tcap);
    k_strlcpy(sub, g_sub, scap);
}

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
    reload_display();
}

void brightness_tray_init(void) {
    g_popup.name = "brightness";
    g_popup.setting = BRIGHTNESS_SETTING;
    g_popup.step = BRIGHTNESS_STEP;
    g_popup.damage = brightness_damage;
    g_popup.hero = hero;
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
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int ch = ugfx_char_h();
    int available = g_popup.unavailable[0] == 0;
    // Wide enough for the sentence when there is one, so it is never
    // clipped; the slider alone wants the shared width.
    int want_w = available ? 0 : ugfx_text_width(g_popup.unavailable) + m.pad * 2;
    int extra_h = available ? 0 : ch + m.vpad;
    if (g_seg_count) extra_h += 1 + m.vpad + m.cap_h + m.btn_h + 4 + ch + m.vpad;
    tray_slider_geometry(&g_popup, want_w, extra_h, m.foot_h, s);
    if (!g) return;

    k_memset(g, 0, sizeof *g);
    g->x = s->x; g->y = s->y; g->w = s->w; g->h = s->h;
    g->tray_x = s->tray_x; g->tray_y = s->tray_y; g->tray_w = s->tray_w; g->tray_h = s->tray_h;
    g->icon_x = s->icon_x; g->icon_y = s->icon_y; g->icon_w = s->icon_w; g->icon_h = s->icon_h;
    g->slider_x = s->slider_x; g->slider_y = s->slider_y;
    g->slider_w = s->slider_w; g->slider_h = s->slider_h;
    g->level = g_popup.level;
    g->available = available;

    int y = s->below_y + (available ? 0 : ch + m.vpad);
    g->seg_count = g_seg_count;
    g->seg_selected = g_seg_selected;
    if (g_seg_count) {
        g->rule_y = y;
        g->cap_y = y + 1 + m.vpad;
        g->seg_y = g->cap_y + m.cap_h;
        g->seg_h = m.btn_h;
        g->seg_x = s->x + m.pad;
        g->seg_w = (s->w - 2 * m.pad) / g_seg_count;
        g->note_y = g->seg_y + m.btn_h + 4;
    }
    g->foot_y = s->foot_y;
    g->foot_h = s->foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (g->foot_h - m.btn_h) / 2;
    g->btn_w = wm_flyout_button_w("Display settings", "tb-gear");
    g->btn_x = s->x + s->w - (m.pad - 6) - g->btn_w;
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
    if (g_hover == TRAY_SLIDER_HOVER_NONE && g_popup.open) {
        struct brightness_geom g;
        geometry(&s, &g);
        for (int i = 0; i < g.seg_count; i++)
            if (uui_hit(g.seg_x + i * g.seg_w, g.seg_y, g.seg_w, g.seg_h, mx, my))
                g_hover = HOVER_SEG(i);
        if (uui_hit(g.btn_x, g.btn_y, g.btn_w, g.btn_h, mx, my)) g_hover = HOVER_BUTTON;
    }
    return g_hover;
}

void brightness_open_now(void) {
    reload_display();
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
    if (what == TRAY_SLIDER_CLICK_INSIDE) {
        struct brightness_geom g;
        geometry(&s, &g);
        for (int i = 0; i < g.seg_count; i++)
            if (uui_hit(g.seg_x + i * g.seg_w, g.seg_y, g.seg_w, g.seg_h, mx, my) &&
                usetting_set(SCALING_SETTING, g_seg_value[i]) > 0) {
                g_seg_selected = i;
                brightness_damage();
            }
        if (uui_hit(g.btn_x, g.btn_y, g.btn_w, g.btn_h, mx, my)) {
            brightness_close();
            { int pid_ = sys_spawn("/bin/wm/system/settings", g.seg_count ? SCALING_SETTING : BRIGHTNESS_SETTING, -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
        }
    }
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
    struct ugfx_surface *surf = wm_surface();
    int ch = ugfx_char_h();

    tray_slider_draw(&g_popup, &s, "tray-brightness", 0);

    if (!g.available)
        ugfx_draw_string_clipped(surf, g.x + s.pad, s.below_y, g.w - 2 * s.pad,
                                 g_popup.unavailable, wm_flyout_dim(), wm_flyout_ink());

    if (g.seg_count) {
        wm_flyout_rule(g.x, g.rule_y, g.w);
        wm_flyout_caption(g.seg_x, g.cap_y, g.w - 2 * s.pad, "SCALING");
        // A SEGMENTED CONTROL: one track, the chosen segment raised on it.
        int tw = g.seg_w * g.seg_count;
        uui_fill_round_rect(surf, g.seg_x, g.seg_y, tw, g.seg_h, 6,
                            ugfx_blend(wm_flyout_ground(), UTHEME_CHROME, 160));
        for (int i = 0; i < g.seg_count; i++) {
            int sx = g.seg_x + i * g.seg_w;
            uint32_t face = ugfx_blend(wm_flyout_ground(), UTHEME_CHROME, 160);
            if (i == g.seg_selected) {
                face = UTHEME_WHITE;
                uui_fill_round_rect(surf, sx + 3, g.seg_y + 3, g.seg_w - 6, g.seg_h - 6, 4, face);
            } else if (g_hover == HOVER_SEG(i)) {
                face = uui_state_bg(face, UUI_STATE_HOVER);
                uui_fill_round_rect(surf, sx + 3, g.seg_y + 3, g.seg_w - 6, g.seg_h - 6, 4, face);
            }
            int lw = ugfx_text_width(g_seg_label[i]);
            if (lw > g.seg_w - 8) lw = g.seg_w - 8;
            ugfx_draw_string_clipped(surf, sx + (g.seg_w - lw) / 2, g.seg_y + (g.seg_h - ch) / 2,
                                     g.seg_w - 8, g_seg_label[i], UTHEME_TEXT, face);
        }
        ugfx_draw_string_clipped(surf, g.seg_x, g.note_y, g.w - 2 * s.pad,
                                 "How a smaller mode fills the panel", wm_flyout_dim(), wm_flyout_ink());
    }

    wm_flyout_button(g.btn_x, g.btn_y, "Display settings", "tb-gear", 0, g_hover == HOVER_BUTTON, 0);
}
