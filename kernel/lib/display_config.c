// The backlight brightness as a registered setting -- sound_config.c's
// shape: this file maps a percent to display_backlight_set() and the
// registry provides the range, the System Settings row, the tray
// flyout's value and `config set brightness` for free.
//
// Registered whether or not the display has a backlight: the SETTING is
// the user's stated preference, and a boot on a machine without the
// hardware must not discard it. `unavailable()` is the sentence a
// client shows instead of a slider there.
//
// THE FLOOR IS NOT ZERO. A duty of 0 turns the backlight off, and a
// persisted 0 would come back on every boot as a black screen the user
// cannot see to fix. Panel off is a separate, unpersisted action.
#include "display_config.h"
#include "display.h"
#include "etc_config.h"
#include "setting.h"
#include "kfmt.h"
#include "knum.h"
#include "klog.h"
#include "initcall.h"
#include "screen.h"
#include "gfx.h"
#include "string.h"

#define DISPLAY_CONFIG_FILE "/etc/toyos.conf"
#define BRIGHTNESS_KEY "brightness"

#define BRIGHTNESS_MIN     5
#define BRIGHTNESS_MAX     100
#define BRIGHTNESS_STEP    5
#define BRIGHTNESS_DEFAULT 100

static int g_brightness = BRIGHTNESS_DEFAULT;

static int parse_pct(const char *value) {
    uint32_t v;
    if (!k_parse_u32(value, &v) || v > BRIGHTNESS_MAX) return -1;
    return (int)v;
}

static void resolution_init(void);

void display_config_init(void) {
    char buf[16];
    if (etc_config_get(DISPLAY_CONFIG_FILE, BRIGHTNESS_KEY, buf, sizeof buf)) {
        int pct = parse_pct(buf);
        if (pct >= BRIGHTNESS_MIN && pct <= BRIGHTNESS_MAX) g_brightness = pct;
    }
    // The display driver claimed long before /etc was readable; only a
    // value somebody stored is pushed at the hardware, so a fresh
    // machine keeps the brightness its firmware chose.
    if (display_has(DISPLAY_CAP_BACKLIGHT) && g_brightness != BRIGHTNESS_DEFAULT)
        display_backlight_set(g_brightness);
    resolution_init();
}
INITCALL(display_config_init, INIT_CONFIG);

static void brightness_get(char *out, uint32_t out_size) {
    // The hardware's own reading when there is one, so a value set by
    // anything else (firmware, a hotkey) reports truthfully.
    int hw = display_backlight_get();
    k_snprintf(out, out_size, "%d", hw >= 0 ? hw : g_brightness);
}

static int brightness_apply(const char *value) {
    int pct = parse_pct(value);
    if (pct < BRIGHTNESS_MIN || pct > BRIGHTNESS_MAX) return SETTING_INVALID;
    g_brightness = pct;
    display_backlight_set(pct);
    char buf[16];
    k_snprintf(buf, sizeof buf, "%d", pct);
    return etc_config_set(DISPLAY_CONFIG_FILE, BRIGHTNESS_KEY, buf)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const char *brightness_unavailable(void) {
    return display_has(DISPLAY_CAP_BACKLIGHT)
               ? 0 : "This display has no backlight control";
}

static const struct setting g_brightness_setting = {
    .name   = BRIGHTNESS_KEY,
    .label  = "Brightness",
    .type   = SETTING_TYPE_INT,
    .file   = DISPLAY_CONFIG_FILE,
    .category = "Display",
    .group  = "Panel",
    .min    = BRIGHTNESS_MIN,
    .max    = BRIGHTNESS_MAX,
    .step   = BRIGHTNESS_STEP,
    .unit   = "%",
    .get    = brightness_get,
    .apply  = brightness_apply,
    .unavailable = brightness_unavailable,
};

// --- the resolution ---------------------------------------------------
//
// An ENUM whose choices are the active driver's mode list, so System
// Settings offers only modes the adapter will show. Stored as "WxH";
// applied at boot from /etc before init starts the desktop, and live
// through screen_set_mode() otherwise.
#define RESOLUTION_KEY "resolution"

static int parse_mode(const char *s, uint32_t *w, uint32_t *h) {
    uint32_t a = 0, b = 0;
    const char *p = s;
    if (!p || !k_isdigit(*p)) return 0;
    while (k_isdigit(*p)) a = a * 10 + (uint32_t)(*p++ - '0');
    if (*p != 'x' && *p != 'X') return 0;
    p++;
    if (!k_isdigit(*p)) return 0;
    while (k_isdigit(*p)) b = b * 10 + (uint32_t)(*p++ - '0');
    if (*p || !a || !b) return 0;
    *w = a; *h = b;
    return 1;
}

static int resolution_choice(int index, char *out, uint32_t out_size) {
    int n = display_mode_count();
    if (n == 0) {   // a fixed-mode display lists exactly its own
        if (index != 0) return 0;
        k_snprintf(out, out_size, "%dx%d", gfx_width(), gfx_height());
        return 1;
    }
    if (index < 0 || index >= n) return 0;
    struct display_mode m;
    display_mode_at(index, &m);
    k_snprintf(out, out_size, "%ux%u", m.width, m.height);
    return 1;
}

static void resolution_get(char *out, uint32_t out_size) {
    k_snprintf(out, out_size, "%dx%d", gfx_width(), gfx_height());
}

static int resolution_apply(const char *value) {
    uint32_t w, h;
    if (!parse_mode(value, &w, &h)) return SETTING_INVALID;
    if (!screen_set_mode(w, h)) return SETTING_INVALID;
    char buf[24];
    k_snprintf(buf, sizeof buf, "%ux%u", w, h);
    return etc_config_set(DISPLAY_CONFIG_FILE, RESOLUTION_KEY, buf)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const char *resolution_unavailable(void) {
    return display_has(DISPLAY_CAP_MODESET)
               ? 0 : "This display cannot change mode after boot";
}

static const struct setting g_resolution_setting = {
    .name   = RESOLUTION_KEY,
    .label  = "Resolution",
    .type   = SETTING_TYPE_ENUM,
    .file   = DISPLAY_CONFIG_FILE,
    .category = "Display",
    .group  = "Screen",
    .choice = resolution_choice,
    .get    = resolution_get,
    .apply  = resolution_apply,
    .unavailable = resolution_unavailable,
};

// At boot: a stored mode is applied before the desktop exists, so it is
// a console re-grid and nothing more. One the driver refuses is logged
// and the boot mode kept.
static void resolution_init(void) {
    char buf[24];
    if (!etc_config_get(DISPLAY_CONFIG_FILE, RESOLUTION_KEY, buf, sizeof buf)) return;
    uint32_t w, h;
    if (!parse_mode(buf, &w, &h)) return;
    if (!screen_set_mode(w, h))
        klog_printf("display: stored resolution %ux%u not applied\n", w, h);
}

void display_config_setting_register(void) {
    setting_register(&g_brightness_setting);
    setting_register(&g_resolution_setting);
}
