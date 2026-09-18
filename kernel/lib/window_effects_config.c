// The desktop's visual effects -- see api/window_effects_config.h.
//
// PERSIST-ONLY, like the window-drag modes beside them in
// /etc/desktop.conf: the registry validates and writes, the compositor
// reads on its generation poll and repaints. Windows exposes the same
// knobs as "Animation effects" and "Transparency effects"; KDE has one
// per effect. One per effect here too, on the Appearance > Effects page.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "window_effects_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static int onoff_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "on", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "off", cap); return 1; }
    return 0;
}

static void shadows_get(char *out, uint32_t cap) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "shadows", out, cap))
        k_strlcpy(out, "on", cap);
}

static const struct setting g_shadows_setting = {
    .name  = "shadows",
    .label = "Window shadows",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Effects",
    .choice = onoff_choice,
    .get   = shadows_get,
    .apply = 0, // persist-only -- wm_shadow.c adopts it
};

void window_effects_setting_register(void) {
    setting_register(&g_shadows_setting);
}
