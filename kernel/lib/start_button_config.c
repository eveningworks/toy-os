// The Start button's appearance: a registry descriptor and nothing else.
//
// PERSIST-ONLY, exactly like the wallpaper beside it and for the same
// reason -- the taskbar is drawn by a RING-3 process, and
// `setting_register()` takes function pointers a process cannot supply.
// The registry validates the value and writes it to /etc; the window
// manager notices on its own generation poll (userland/wm/wm_taskbar.c).
// It shares /etc/desktop.conf with the wallpaper, which is what makes it
// `desktop.start_button` -- the namespace is the registered name of the
// file (kernel/lib/config_file.c), not something declared here.
//
// WHY THREE CHOICES AND NOT A BOOLEAN. `text`, `icon`, `both` is what
// XFCE's Whisker Menu offers verbatim (Icon / Title / Icon and title)
// and what KDE's Application Launcher exposes as "Icon and text"
// against its icon-only default. A boolean cannot say `both`, which is
// the combination Windows 95 through 7 shipped and the one most people
// picture when they hear "Start button". Two settings (a mode and a
// name for the artwork) was the other option and was not taken: the
// button has exactly one mark, and a second setting would be a second
// thing that can point at a file that is not there.
//
// THE DEFAULT IS `text`, so a machine with nothing written looks
// exactly as it did before this existed. That is deliberate rather than
// a preference about which looks better: the Start button's width is
// derived from what is IN it, so changing the default would move every
// taskbar button on every screenshot-based GUI test at once.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "start_button_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

// Ordered as they escalate -- text, then icon, then both -- so a UI
// listing them reads as a progression rather than an arbitrary set.
static const char *const g_modes[] = { "text", "icon", "both" };

static int mode_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_modes / sizeof g_modes[0])) return 0;
    k_strlcpy(out, g_modes[index], out_size);
    return 1;
}

// Reads the FILE, not the compositor's adopted value -- the same note
// wallpaper_config.c's own getter carries: the two differ only in the
// window between a write and the next poll, and the file is what the
// registry means by "what is this set to".
static void mode_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "start_button", out, out_size)) {
        k_strlcpy(out, g_modes[0], out_size);
    }
}

static const struct setting g_start_button_setting = {
    .name = "start_button",
    .label = "Start button",
    .type = SETTING_TYPE_ENUM,
    .file = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Desktop",
    .choice = mode_choice,
    .get = mode_get,
    .apply = 0, // persist-only -- the taskbar's poll applies it
};

void start_button_setting_register(void) {
    setting_register(&g_start_button_setting);
}
