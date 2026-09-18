// desktop.smooth_scroll -- see api/smooth_scroll_config.h.
//
// PERSIST-ONLY, like the window-drag modes beside it in
// /etc/desktop.conf: the registry validates and writes the value, and
// every Toykit widget asks for it when a scroll begins, so a change in
// System Settings applies to the next notch in every open window.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "smooth_scroll_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static int onoff_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "on", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "off", cap); return 1; }
    return 0;
}

static void smooth_get(char *out, uint32_t cap) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "smooth_scroll", out, cap))
        k_strlcpy(out, "on", cap);
}

static const struct setting g_smooth_setting = {
    .name  = "smooth_scroll",
    .label = "Smooth scrolling",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Effects",
    .choice = onoff_choice,
    .get   = smooth_get,
    .apply = 0, // persist-only -- the toolkit adopts it per scroll
};

void smooth_scroll_setting_register(void) {
    setting_register(&g_smooth_setting);
}
