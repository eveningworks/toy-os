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

static void animations_get(char *out, uint32_t cap) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "animations", out, cap))
        k_strlcpy(out, "on", cap);
}

static const struct setting g_animations_setting = {
    .name  = "animations",
    .label = "Window animations",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Effects",
    .choice = onoff_choice,
    .get   = animations_get,
    .apply = 0, // persist-only -- wm_anim.c adopts it
};

// ONE SPEED FOR EVERY ANIMATION, which is KWin's Animation Speed
// slider rather than a duration per effect. macOS exposes the effect
// but not the speed; Windows exposes neither. Named steps rather than
// milliseconds: "250 ms" in a settings page is a developer's unit, and
// the compositor is free to give different effects different natural
// lengths under the same multiplier.
static int speed_choice(int index, char *out, uint32_t cap) {
    static const char *const NAMES[] = { "instant", "fast", "normal",
                                         "slow", "very-slow" };
    if (index < 0 || index >= (int)(sizeof NAMES / sizeof NAMES[0])) return 0;
    k_strlcpy(out, NAMES[index], cap);
    return 1;
}

static void animation_speed_get(char *out, uint32_t cap) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "animation_speed", out, cap))
        k_strlcpy(out, "normal", cap);
}

static const struct setting g_animation_speed_setting = {
    .name  = "animation_speed",
    .label = "Animation speed",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Effects",
    .choice = speed_choice,
    .get   = animation_speed_get,
    .apply = 0, // persist-only -- wm_anim.c adopts it
};

// WHICH EFFECT a minimize and its restore use. macOS asks exactly this
// -- "Minimize windows using: Genie / Scale" -- and KWin ships the same
// set as pluggable effects. Open and close are deliberately NOT
// covered: a window that genies out of nothing reads as a glitch, and
// macOS applies its genie to the Dock alone.
static int minimize_effect_choice(int index, char *out, uint32_t cap) {
    static const char *const NAMES[] = { "scale", "genie", "squash", "glide" };
    if (index < 0 || index >= (int)(sizeof NAMES / sizeof NAMES[0])) return 0;
    k_strlcpy(out, NAMES[index], cap);
    return 1;
}

static void minimize_effect_get(char *out, uint32_t cap) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "minimize_effect", out, cap))
        k_strlcpy(out, "scale", cap);
}

static const struct setting g_minimize_effect_setting = {
    .name  = "minimize_effect",
    .label = "Minimize effect",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Effects",
    .choice = minimize_effect_choice,
    .get   = minimize_effect_get,
    .apply = 0, // persist-only -- wm_anim.c adopts it
};

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
    setting_register(&g_animations_setting);
    setting_register(&g_animation_speed_setting);
    setting_register(&g_minimize_effect_setting);
    setting_register(&g_shadows_setting);
}
