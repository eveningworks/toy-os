// See volume_popup.h for what this is and why the panel owns it. The
// slider row, the debounced write and the overlay verbs are
// tray_slider_popup.c's; what is here is only the volume's own -- mute,
// the device rows, and the speaker icon that follows the level.
#include "wm_internal.h"
#include "volume_popup.h"
#include "tray_slider_popup.h"
#include "wm_tray.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "lib/usetting.h"

#define VOLUME_SETTING "system.volume"
#define DEVICE_SETTING "system.audio_device"

// One wheel notch, and one arrow-sized step. Five is what KDE, GNOME
// and Windows all move per notch.
#define VOLUME_STEP 5

int volume_open = 0;

static struct tray_slider_popup g_popup;
static const char *g_tray_icon;
static int g_premute_level = 100;      // what unmuting goes back to

// The device list, adopted from the setting's CHOICE list. Copied
// rather than re-read per frame: drawing runs on every mouse move over
// an open panel, and each row would otherwise be a syscall.
static char g_dev_value[VOLUME_MAX_DEVICES][SETTING_ABI_VALUE_MAX];
static char g_dev_label[VOLUME_MAX_DEVICES][SETTING_ABI_LABEL_MAX];
static int  g_dev_count;
static int  g_dev_selected;            // which row carries the tick

// The overlay registry's hover token: the shared row's, or a device
// row from TRAY_SLIDER_HOVER_OWNER up. Compared by the core, read by
// the draw -- see wm_overlay.h.
static int g_hover;

// --- the settings behind it -------------------------------------------

// The device rows ARE the `audio_device` setting's choice list, which
// is why nothing here knows what a sound card is: the kernel's setting
// computes its choices from the registered drivers, so a card plugged
// in after boot turns up as a row.
static void reload_devices(void) {
    struct setting_msg m;
    int index = usetting_find(DEVICE_SETTING, &m);
    g_dev_count = 0;
    if (index < 0) return;

    char current[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(DEVICE_SETTING, current, sizeof current))
        k_strlcpy(current, "auto", sizeof current);

    for (int c = 0; c < VOLUME_MAX_DEVICES; c++) {
        k_memset(&m, 0, sizeof m);
        m.op = SETTING_OP_CHOICE;
        m.index = index;
        m.choice = c;
        // **THE INDEX CAME FROM usetting_find(), SO THE CHOICE MUST
        // GO BACK THROUGH THE SAME REGISTRY.** They agree today only
        // because the merged list puts the kernel's settings first and
        // this is one of them; a declared setting's index means nothing
        // to the syscall.
        if (usetting_dispatch(&m) != 0) break;
        k_strlcpy(g_dev_value[g_dev_count], m.value, SETTING_ABI_VALUE_MAX);
        k_strlcpy(g_dev_label[g_dev_count], m.label, SETTING_ABI_LABEL_MAX);
        if (k_strcmp(m.value, current) == 0) g_dev_selected = g_dev_count;
        g_dev_count++;
    }
    if (g_dev_selected >= g_dev_count) g_dev_selected = 0;
}

// --- the tray item ----------------------------------------------------

static const char *icon_for(int level) {
    if (level <= 0) return "tray-volume-muted";
    if (level < 50) return "tray-volume-low";
    return "tray-volume-high";
}

void volume_tray_update(void) {
    if (g_popup.tray_id < 0) return;
    const char *want = icon_for(g_popup.level);
    if (want == g_tray_icon) return;
    g_tray_icon = want;
    tray_set_icon(g_popup.tray_id, want);
}

// After any change of level: remember what unmuting returns to, and
// let the speaker icon follow.
static void on_level(void) {
    if (g_popup.level > 0) g_premute_level = g_popup.level;
    volume_tray_update();
}

void volume_tray_init(void) {
    g_popup.name = "volume";
    g_popup.setting = VOLUME_SETTING;
    g_popup.step = VOLUME_STEP;
    g_popup.on_level = on_level;
    g_popup.on_reload = reload_devices;
    g_popup.damage = volume_damage;
    reload_devices();
    g_tray_icon = icon_for(100);
    tray_slider_init(&g_popup, g_tray_icon);
    volume_tray_update();   // the registered icon follows the level read
}

// --- geometry ---------------------------------------------------------

// The one geometry: the shared row from tray_slider_geometry(), sized
// for the device rows below it, and the rows themselves. `g` may be
// NULL when only the row is wanted.
static void geometry(struct tray_slider_geom *s, struct volume_geom *g) {
    int ch = ugfx_char_h();
    int pad = ch / 2 + 2;
    int row_h = ch + 8;
    int rows = g_dev_count;
    // Wide enough for the widest device label rather than a constant:
    // "Automatic (usb-audio)" is longer than anything else here, and a
    // panel sized for the shorter case clips it.
    int want_w = ugfx_text_width("Automatic (usb-audio)") + pad * 4;
    int extra_h = pad / 2 + 1 + pad / 2     // the rule
                  + ch + pad / 2            // "Output device"
                  + rows * row_h;
    tray_slider_geometry(&g_popup, want_w, extra_h, s);
    if (!g) return;

    k_memset(g, 0, sizeof *g);
    g->x = s->x; g->y = s->y; g->w = s->w; g->h = s->h;
    g->tray_x = s->tray_x; g->tray_y = s->tray_y; g->tray_w = s->tray_w; g->tray_h = s->tray_h;
    g->mute_x = s->icon_x; g->mute_y = s->icon_y; g->mute_w = s->icon_w; g->mute_h = s->icon_h;
    g->slider_x = s->slider_x; g->slider_y = s->slider_y;
    g->slider_w = s->slider_w; g->slider_h = s->slider_h;
    g->list_x = s->x + pad;
    g->list_y = s->below_y + pad + 1 + ch + pad / 2;
    g->row_h = row_h;
    g->rows = rows;
    g->level = g_popup.level;
    g->muted = (g_popup.level == 0);
    g->selected_row = g_dev_selected;
}

void volume_geometry(struct volume_geom *g) {
    struct tray_slider_geom s;
    geometry(&s, g);
}

static void slider_geom(struct tray_slider_geom *s) { geometry(s, 0); }

int volume_row(int index, char *value, uint32_t value_size,
               char *label, uint32_t label_size) {
    if (index < 0 || index >= g_dev_count) return 0;
    if (value) k_strlcpy(value, g_dev_value[index], value_size);
    if (label) k_strlcpy(label, g_dev_label[index], label_size);
    return 1;
}

// --- state ------------------------------------------------------------

void volume_damage(void) {
    struct volume_geom g;
    volume_geometry(&g);
    wm_damage_rect(g.x, g.y, g.w, g.h);
    redraw_pending = 1;
}

static int row_at(const struct volume_geom *g, int mx, int my) {
    for (int i = 0; i < g->rows; i++)
        if (uui_hit(g->list_x, g->list_y + i * g->row_h,
                    g->w - (g->list_x - g->x) * 2, g->row_h, mx, my))
            return i;
    return -1;
}

int volume_hover_at(int mx, int my) {
    struct tray_slider_geom s;
    slider_geom(&s);
    g_hover = tray_slider_hover_at(&g_popup, &s, mx, my);
    if (g_hover == TRAY_SLIDER_HOVER_NONE && g_popup.open) {
        struct volume_geom g;
        volume_geometry(&g);
        int row = row_at(&g, mx, my);
        if (row >= 0) g_hover = TRAY_SLIDER_HOVER_OWNER + row;
    }
    return g_hover;
}

void volume_open_now(void) {
    tray_slider_open(&g_popup);
    volume_open = g_popup.open;
    g_hover = TRAY_SLIDER_HOVER_NONE;
}

void volume_close(void) {
    tray_slider_close(&g_popup);
    volume_open = g_popup.open;
    g_hover = TRAY_SLIDER_HOVER_NONE;
}

void volume_poll_config(void) {
    tray_slider_poll(&g_popup);
}

// --- input ------------------------------------------------------------

int volume_handle_click(int mx, int my) {
    struct tray_slider_geom s;
    slider_geom(&s);
    enum tray_slider_click what = tray_slider_click(&g_popup, &s, mx, my);
    volume_open = g_popup.open;

    switch (what) {
    case TRAY_SLIDER_CLICK_NONE:
        return 0;
    case TRAY_SLIDER_CLICK_DISMISSED:
        // A dismissing click on the TASKBAR falls through, so the Start
        // button acts on the same click that closed this -- and so a
        // second click on our own tray item closes rather than
        // reopening. Anything else is swallowed, as for any open menu.
        return my < screen_h - taskbar_h;
    case TRAY_SLIDER_CLICK_ICON:
        // MUTE IS A LEVEL OF ZERO, not a separate flag: the registry
        // holds one number and a second piece of state would have to
        // persist beside it. The pre-mute level is remembered here, so
        // unmuting returns to it within this session -- a reboot while
        // muted comes back at zero, which is the honest cost.
        tray_slider_set_level(&g_popup,
                              g_popup.level == 0 ? (g_premute_level ? g_premute_level : VOLUME_STEP)
                                                 : 0, 1);
        return 1;
    case TRAY_SLIDER_CLICK_INSIDE: {
        struct volume_geom g;
        volume_geometry(&g);
        int i = row_at(&g, mx, my);
        if (i >= 0 && usetting_set(DEVICE_SETTING, g_dev_value[i]) > 0) {
            g_dev_selected = i;
            // The labels move with the choice: "Automatic (ac97)" names
            // what auto resolved to, and that changes when the pick does.
            reload_devices();
            redraw_pending = 1;
        }
        return 1;   // a click inside the panel never falls through
    }
    default:
        return 1;
    }
}

void volume_update_press(int mx, int my, uint8_t buttons) {
    (void)my;
    if (!g_popup.scale.dragging) return;
    struct tray_slider_geom s;
    slider_geom(&s);   // places the scale before the drag reads it
    tray_slider_update_press(&g_popup, mx, buttons);
}

int volume_handle_wheel(int mx, int my, int notches) {
    struct tray_slider_geom s;
    slider_geom(&s);
    return tray_slider_wheel(&g_popup, &s, mx, my, notches);
}

// --- drawing ----------------------------------------------------------

void volume_draw(int mx, int my) {
    if (!volume_open) return;
    // The TRACKED hover, not one derived from (mx, my): only that one
    // has damaged the panel when it changed (wm_overlay.h).
    (void)mx; (void)my;

    struct tray_slider_geom s;
    struct volume_geom g;
    geometry(&s, &g);

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    uint32_t accent = UTHEME_ACCENT;
    uint32_t hover_bg = uui_state_bg(bg, UUI_STATE_HOVER);

    tray_slider_draw(&g_popup, &s, icon_for(g.level), g_hover == TRAY_SLIDER_HOVER_ICON);

    // --- the rule and the heading -------------------------------------
    int rule_y = g.mute_y + g.mute_h + 4;
    ugfx_fill_rect(wm_surface(), g.x + 8, rule_y, g.w - 16, 1, border);
    ugfx_draw_string_clipped(wm_surface(), g.list_x, rule_y + 6,
                             g.w - 16, "Output device", border, bg);

    // --- the devices ---------------------------------------------------
    for (int i = 0; i < g.rows; i++) {
        int ry = g.list_y + i * g.row_h;
        int rw = g.w - (g.list_x - g.x) * 2;
        int over = (g_hover == TRAY_SLIDER_HOVER_OWNER + i);
        uint32_t row_bg = bg, row_fg = fg;
        if (i == g.selected_row) { row_bg = accent; row_fg = UTHEME_ACCENT_TEXT; }
        else if (over)           { row_bg = hover_bg; }
        ugfx_fill_rect(wm_surface(), g.list_x, ry, rw, g.row_h, row_bg);
        ugfx_draw_string_clipped(wm_surface(), g.list_x + 6,
                                 ry + (g.row_h - ugfx_char_h()) / 2,
                                 rw - 12, g_dev_label[i], row_fg, row_bg);
    }
}
