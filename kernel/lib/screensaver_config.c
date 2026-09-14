// THE SCREENSAVER'S TWO SETTINGS, and nothing else.
//
// PERSIST-ONLY, like the taskbar height and the wallpaper beside them in
// /etc/desktop.conf: the compositor owns the idle clock and spawns the
// saver, so the registry's whole job is to validate a value and write
// it. `userland/wm/wm_idle.c` adopts both on its generation poll.
//
// WHICH SAVER IS THE NAME OF A PROGRAM, and the choice list is the
// DIRECTORY those programs live in (api/setting.h's `choice_dir`), so
// dropping a `.c` into `userland/gui/savers/` gives it a row in System
// Settings with no edit here. That is Windows' arrangement, where a
// screensaver is a `.scr` the shell runs, rather than GNOME's, where
// the saver is part of the shell.
//
// THE TIMEOUT DISABLES IT. Zero minutes means never, so there is no
// separate enable that can disagree with it -- the invalid combination
// simply cannot be expressed.
#include "setting.h"
#include "etc_config.h"
#include "kfmt.h"
#include "string.h"
#include "screensaver_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static void saver_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "screensaver", out, out_size))
        k_strlcpy(out, SCREENSAVER_DEFAULT, out_size);
}

static void idle_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "screensaver_idle", out, out_size))
        k_snprintf(out, out_size, "%d", SCREENSAVER_IDLE_DEFAULT);
}

static const struct setting g_saver_setting = {
    .name  = "screensaver",
    .label = "Screensaver",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Desktop",
    .group    = "Screensaver",
    .choice_dir = SCREENSAVER_DIR,
    .get   = saver_get,
    .apply = 0, // persist-only -- wm_idle.c adopts it
};

static const struct setting g_idle_setting = {
    .name  = "screensaver_idle",
    .label = "Start after",
    .type  = SETTING_TYPE_INT,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Desktop",
    .group    = "Screensaver",
    .min  = 0,
    .max  = SCREENSAVER_IDLE_MAX,
    .step = 1,
    .unit = "min",
    .get   = idle_get,
    .apply = 0,
};

void screensaver_setting_register(void) {
    setting_register(&g_saver_setting);
    setting_register(&g_idle_setting);
}
