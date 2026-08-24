// The desktop wallpaper's registry descriptors.
//
// A wallpaper is a picture decoded and drawn by a RING-3 process (see
// userland/lib/uimg.h on why no image parser is in this kernel), so
// nothing about it is the kernel's business -- except what the SETTING
// is. `setting_register()` takes function pointers and a ring-3 process
// cannot supply one, so both descriptors are PERSIST-ONLY (`.apply` is
// 0): the registry validates a value and writes it to /etc, and the
// desktop notices on its own. That is exactly the arrangement
// cursor_theme_config.c documents, and this file is deliberately shaped
// after it -- a setting that looks different from its neighbours is one
// a reader has to re-learn.
//
// WHY THE VALUE IS A NAME AND NOT A PATH. `aurora`, not
// `/usr/share/wallpapers/aurora.jpg` -- the same rule a font face
// (`fontface`) and a cursor theme already follow here: a user-selectable
// resource is a file in a known directory, named by its filename without
// the extension, so the choice list is a directory listing rather than
// something that has to be maintained. It also makes the setting an ENUM
// with real choices, which is what gives it a System Settings row that
// works.
//
// WHAT IT DOES NOT DO IS VALIDATE. The registry stores whatever it is
// given, exactly as it does for a cursor theme -- `config set
// desktop.wallpaper nosuchimage` is accepted, and the desktop logs
// `wallpaper .../nosuchimage.jpg not shown -- no such file` and falls
// back to its plain colour. That is deliberate consistency with the
// neighbour rather than a gap: a setting whose owner is a ring-3
// process cannot be checked by the kernel at set time without the
// kernel learning what a wallpaper is.
//
// The cost, stated rather than hidden: a picture somewhere else on the
// disk cannot be the wallpaper until it is copied into
// /usr/share/wallpapers. Image Viewer says so rather than failing
// quietly.
#include "setting.h"
#include "etc_config.h"
#include "fs.h"
#include "string.h"
#include "wallpaper_config.h"

#define WALLPAPER_CONFIG_FILE "/etc/desktop.conf"
#define WALLPAPER_DIR         "/usr/share/wallpapers"

// "none" is a real choice and is FIRST, so it is also the answer a
// machine with an empty wallpaper directory gives -- a dropdown with no
// options at all would be a control the user cannot use to get out of
// whatever state they are in.
#define WALLPAPER_NONE "none"

// fs_list() walks through a callback with no context pointer, so the
// index being looked for and the name found travel through these. Same
// constraint, and the same acceptance of it, as cursor_theme_config.c:
// used only inside wallpaper_choice(), which does not re-enter.
static int g_want_index;
static int g_seen_index;
static char g_found[SETTING_VALUE_MAX];

// Strips the extension, so `aurora.jpg` is offered as `aurora`.
static void put_stem(const char *name) {
    k_strlcpy(g_found, name, sizeof g_found);
    for (int i = 0; g_found[i]; i++) {
        if (g_found[i] == '.') { g_found[i] = '\0'; return; }
    }
}

static void wallpaper_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (is_dir) return;
    if (g_seen_index == g_want_index) put_stem(name);
    g_seen_index++;
}

static int wallpaper_choice(int index, char *out, uint32_t out_size) {
    if (index == 0) {
        k_strlcpy(out, WALLPAPER_NONE, out_size);
        return 1;
    }
    g_want_index = index - 1;
    g_seen_index = 0;
    g_found[0] = '\0';
    fs_list(WALLPAPER_DIR, wallpaper_cb);
    if (!g_found[0]) return 0;
    k_strlcpy(out, g_found, out_size);
    return 1;
}

// Reads the FILE, not any live state -- the desktop's adopted value is
// the same thing except in the window between a write and its next poll,
// and reading the file is what the registry means by "what is this set
// to" (cursor_theme_config.c's own note says this first).
//
// THE DEFAULT IS A NAME THAT MAY NOT EXIST, and that is deliberate: an
// image missing from the disk leaves the desktop on its plain colour
// with a log line saying why, which is a better failure than a boot with
// no default at all.
static void wallpaper_get(char *out, uint32_t out_size) {
    if (!etc_config_get(WALLPAPER_CONFIG_FILE, "wallpaper", out, out_size)) {
        k_strlcpy(out, "aurora", out_size);
    }
}

static const char *const g_modes[] = { "fill", "fit" };

static int mode_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_modes / sizeof g_modes[0])) return 0;
    k_strlcpy(out, g_modes[index], out_size);
    return 1;
}

static void mode_get(char *out, uint32_t out_size) {
    if (!etc_config_get(WALLPAPER_CONFIG_FILE, "wallpaper_mode", out, out_size)) {
        k_strlcpy(out, g_modes[0], out_size);
    }
}

static const struct setting g_wallpaper_setting = {
    .name = "wallpaper",
    .label = "Wallpaper",
    .type = SETTING_TYPE_ENUM,
    .file = WALLPAPER_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Desktop",
    .choice = wallpaper_choice,
    .get = wallpaper_get,
    .apply = 0, // persist-only -- the desktop's poll applies it
};

static const struct setting g_mode_setting = {
    .name = "wallpaper_mode",
    .label = "Wallpaper placement",
    .type = SETTING_TYPE_ENUM,
    .file = WALLPAPER_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Desktop",
    .choice = mode_choice,
    .get = mode_get,
    .apply = 0,
};

// ---- desktop.layout_log ---------------------------------------------
//
// **A DIAGNOSTIC A HUMAN SHOULD NOT BE DROWNED IN.** Every Toykit app
// logs its widget geometry so a GUI test can drive it by asking rather
// than by guessing pixels (uapp_log_layout()), and it did so on EVERY
// FRAME, unconditionally, to the kernel log. Nine apps, twenty-odd call
// sites. The result was that `dmesg` on a machine with a window open
// was mostly one app repeating itself, and `dmesg -w` was a FEEDBACK
// LOOP: printing a line moved the Terminal's cursor, which redrew,
// which logged the new cursor, which printed a line.
//
// Off by default for the same reason `kernel.kbdtap` is (tunables.c):
// the cost of recording is trivial, and the default is about what the
// machine SHOWS, not about what it costs. Persist-only -- each app
// reads it once at startup, so a test sets it before launching what it
// intends to watch, which `enter_gui()` does for every tool at once.
#define LAYOUT_LOG_DEFAULT "off"

static const char *const g_onoff[] = { "off", "on" };

static int layout_log_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_onoff / sizeof g_onoff[0])) return 0;
    k_strlcpy(out, g_onoff[index], out_size);
    return 1;
}

static void layout_log_get(char *out, uint32_t out_size) {
    if (!etc_config_get(WALLPAPER_CONFIG_FILE, "layout_log", out, out_size))
        k_strlcpy(out, LAYOUT_LOG_DEFAULT, out_size);
}

static const struct setting g_layout_log_setting = {
    .name = "layout_log",
    .label = "Log widget geometry for GUI tests",
    .type = SETTING_TYPE_ENUM,
    .file = WALLPAPER_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Diagnostics",
    .choice = layout_log_choice,
    .get = layout_log_get,
    .apply = 0, // persist-only -- an app reads it when it starts
};

void wallpaper_setting_register(void) {
    setting_register(&g_wallpaper_setting);
    setting_register(&g_mode_setting);
    setting_register(&g_layout_log_setting);
}
