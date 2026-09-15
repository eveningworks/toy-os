// Which notification-area items are shown: a registry descriptor per
// item and nothing else.
//
// PERSIST-ONLY, like the wallpaper and the Start button beside it in
// /etc/desktop.conf and for the same reason -- the tray is drawn by a
// RING-3 process, and `setting_register()` takes function pointers a
// process cannot supply. The registry validates and writes; the window
// manager adopts the value on its generation poll.
//
// THREE VALUES, NOT A CHECKBOX. `auto` is the interesting one: it means
// "show this if the hardware behind it exists", which is the only answer
// that can be right on both a laptop with a backlight and a QEMU guest
// without one. Windows 11 (Taskbar > System tray icons) and Plasma's
// panel both offer exactly this three-way per item, and for the same
// reason -- a user who wants a control pinned regardless outranks the
// hardware probe, and so does one who never wants to see it.
//
// `always` IS ALSO WHAT KEEPS THE HIDDEN ITEM TESTED. Every GUI tool
// here runs under QEMU, where no adapter has a backlight, so an item
// that hid itself on `auto` would leave its flyout's drawing, geometry,
// dismissal and mutual exclusion exercised by nobody -- the exact shape
// of "a green suite that tests nothing" the decision to show it
// unconditionally was avoiding (docs/decisions.md). A test sets
// `always` and keeps every check.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "tray_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static const char *const g_modes[] = {
    TRAY_SHOW_AUTO, TRAY_SHOW_ALWAYS, TRAY_SHOW_NEVER
};

static int mode_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_modes / sizeof g_modes[0])) return 0;
    k_strlcpy(out, g_modes[index], out_size);
    return 1;
}

// Reads the FILE, not the compositor's adopted value -- the same note
// week_start_config.c's getter carries: the two differ only in the
// window between a write and the next poll, and the file is what the
// registry means by "what is this set to".
//
// One getter per setting because `get` takes no context. Two functions
// is cheaper to read than a macro that hides which key each one names.
static void brightness_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "tray_brightness", out, out_size)) {
        k_strlcpy(out, g_modes[0], out_size);
    }
}

static void network_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "tray_network", out, out_size)) {
        k_strlcpy(out, g_modes[0], out_size);
    }
}

static void remote_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "tray_remote", out, out_size)) {
        k_strlcpy(out, g_modes[0], out_size);
    }
}

static const struct setting g_tray_brightness_setting = {
    .name = "tray_brightness",
    .label = "Brightness",
    .type = SETTING_TYPE_ENUM,
    .file = DESKTOP_CONFIG_FILE,
    .category = "Desktop",
    .group = "Tray",
    .choice = mode_choice,
    .get = brightness_get,
    .apply = 0, // persist-only -- the brightness item's poll applies it
};

static const struct setting g_tray_network_setting = {
    .name = "tray_network",
    .label = "Network",
    .type = SETTING_TYPE_ENUM,
    .file = DESKTOP_CONFIG_FILE,
    .category = "Desktop",
    .group = "Tray",
    .choice = mode_choice,
    .get = network_get,
    .apply = 0, // persist-only -- the network item's poll applies it
};

static const struct setting g_tray_remote_setting = {
    .name = "tray_remote",
    .label = "Remote activity",
    .type = SETTING_TYPE_ENUM,
    .file = DESKTOP_CONFIG_FILE,
    .category = "Desktop",
    .group = "Tray",
    .choice = mode_choice,
    .get = remote_get,
    .apply = 0, // persist-only -- the remote item's poll applies it
};

void tray_setting_register(void) {
    setting_register(&g_tray_brightness_setting);
    setting_register(&g_tray_network_setting);
    setting_register(&g_tray_remote_setting);
}
