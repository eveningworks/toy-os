// Which day a week starts on: a registry descriptor and nothing else.
//
// PERSIST-ONLY, exactly like the wallpaper and the Start button beside
// it and for the same reason -- the taskbar's calendar popup is drawn
// by a RING-3 process, and `setting_register()` takes function pointers
// a process cannot supply. The registry validates the value and writes
// it to /etc; the window manager notices on its own generation poll
// (userland/wm/calendar_popup.c). It shares /etc/desktop.conf with the
// wallpaper, which is what makes it `desktop.week_start` -- the
// namespace is the registered name of the file (kernel/lib/config_file.c),
// not something declared here.
//
// WHY A SETTING AND NOT A CONSTANT. Every system that draws a month
// grid has this control and none of them hardcode it: GNOME and KDE
// both take it from the locale's `first_weekday` with an explicit
// override, and Windows exposes it in Region settings. toy-os has no
// locale to derive it from, so the override IS the setting. Two choices
// rather than seven: `cal(1)` offers Monday and Sunday, ISO 8601 says
// Monday, the US and Windows default to Sunday, and nothing here has a
// use for a week that starts on a Wednesday.
//
// THE DEFAULT IS `monday` (ISO 8601). It is the one an empty
// /etc/desktop.conf gets, so the grid's column order is a stated
// default rather than whatever the code happened to do first.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "week_start_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static const char *const g_days[] = { "monday", "sunday" };

static int day_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_days / sizeof g_days[0])) return 0;
    k_strlcpy(out, g_days[index], out_size);
    return 1;
}

// Reads the FILE, not the compositor's adopted value -- the same note
// wallpaper_config.c's and start_button_config.c's getters carry: the
// two differ only in the window between a write and the next poll, and
// the file is what the registry means by "what is this set to".
static void day_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "week_start", out, out_size)) {
        k_strlcpy(out, g_days[0], out_size);
    }
}

static const struct setting g_week_start_setting = {
    .name = "week_start",
    .label = "First day",
    .type = SETTING_TYPE_ENUM,
    .file = DESKTOP_CONFIG_FILE,
    .category = "Desktop",
    .group = "Calendar",
    .choice = day_choice,
    .get = day_get,
    .apply = 0, // persist-only -- the calendar popup's poll applies it
};

void week_start_setting_register(void) {
    setting_register(&g_week_start_setting);
}
