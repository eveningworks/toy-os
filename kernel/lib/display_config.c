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
#include "initcall.h"

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

void display_config_setting_register(void) {
    setting_register(&g_brightness_setting);
}
