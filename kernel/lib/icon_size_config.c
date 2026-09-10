// The desktop's icon size: a registry descriptor and nothing else.
//
// PERSIST-ONLY, like the wallpaper and the week start beside it: the
// desktop is drawn by a ring-3 process and applies the value on its own
// generation poll (userland/wm/desktop.c). It shares /etc/desktop.conf,
// which is what makes it `desktop.icon_size`.
//
// THREE NAMED SIZES, NOT A PIXEL COUNT. Windows offers Small/Medium/
// Large on the desktop's View menu; KDE's Folder View has a size slider
// but its Icons menu still names steps. Named sizes are what a context
// menu can list with a tick, and what an ordered enum's radio/slider in
// System Settings draws unaided. The pixel value of each name is the
// desktop's business (32/48/64 today), not the registry's.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "icon_size_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static const char *const g_sizes[] = { "small", "medium", "large" };

static int size_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_sizes / sizeof g_sizes[0])) return 0;
    k_strlcpy(out, g_sizes[index], out_size);
    return 1;
}

// Reads the FILE, not the compositor's adopted value (week_start_config.c
// has the note). The default is `medium`, the 48 px the desktop always drew.
static void size_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "icon_size", out, out_size)) {
        k_strlcpy(out, g_sizes[1], out_size);
    }
}

static const struct setting g_icon_size_setting = {
    .name = "icon_size",
    .label = "Icon size",
    .type = SETTING_TYPE_ENUM,
    .file = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Desktop",
    .choice = size_choice,
    .get = size_get,
    .apply = 0, // persist-only -- the desktop's poll applies it
};

void icon_size_setting_register(void) {
    setting_register(&g_icon_size_setting);
}
