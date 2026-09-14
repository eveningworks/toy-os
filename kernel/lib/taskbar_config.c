// The taskbar's thickness: a registry descriptor and nothing else.
//
// PERSIST-ONLY, like the Start button and the wallpaper beside it in
// /etc/desktop.conf and for the same reason: the strip is drawn by a
// ring-3 process, so the registry validates and writes, and the window
// manager adopts the value on its generation poll.
//
// A PIXEL COUNT, not a small/medium/large enum: that is XFCE's "Row size
// (pixels)" spinbox and the number KDE stores for a panel's height
// (Windows 11 fixes its bar at 48 and GNOME does not expose one). The
// bounds are the useful range -- below 24 the button loses its label
// and its icon, above 96 an icon scaled from a 64px master is upscaled.
//
#include "setting.h"
#include "etc_config.h"
#include "kfmt.h"
#include "string.h"
#include "taskbar_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static void height_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "taskbar_height", out, out_size)) {
        k_snprintf(out, out_size, "%d", TASKBAR_H_DEFAULT);
    }
}

static const struct setting g_taskbar_height_setting = {
    .name = "taskbar_height",
    .label = "Taskbar height",
    .type = SETTING_TYPE_INT,
    .file = DESKTOP_CONFIG_FILE,
    .category = "Desktop",
    .min  = TASKBAR_H_MIN,
    .max  = TASKBAR_H_MAX,
    .step = TASKBAR_H_STEP,
    .unit = "px",
    .get = height_get,
    .apply = 0, // persist-only -- the taskbar's poll applies it
};

void taskbar_setting_register(void) {
    setting_register(&g_taskbar_height_setting);
}
