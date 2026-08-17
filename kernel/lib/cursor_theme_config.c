// The cursor THEME and SIZE settings' registry descriptors.
//
// The pointer's shapes are drawn by the compositor and its themes are
// data files, so nothing about a cursor theme is the kernel's business
// -- except this: what the SETTING is. `setting_register()` takes
// function pointers, and a ring-3 process cannot supply one, so a
// setting owned by a ring-3 compositor would otherwise need the kernel
// to call back into it, which is precisely the inversion Milestone 41
// exists to avoid.
//
// It does not need to. Both descriptors are **persist-only** (`.apply`
// is 0): the registry validates a value and writes it to `/etc`, and
// whoever owns the thing notices by watching `setting_generation()`.
// The compositor's `cursor_theme_poll()` already does exactly that. So
// the kernel owns the DESCRIPTION -- name, label, legal values, which
// file -- and the compositor owns the BEHAVIOUR, which is the same
// split every other setting here has.
//
// This is the file `apps/wm/cursor_theme.c` used to hold these in, and
// its comment there already predicted the move: "registering an apply
// callback here would have to be undone then". It was right.
//
// Shaped after font_config.c/cursor_config.c, deliberately: a setting
// that looks different from its neighbours is one a reader has to
// re-learn.
#include "setting.h"
#include "etc_config.h"
#include "fs.h"
#include "string.h"

#define CURSOR_CONFIG_FILE "/etc/toyos.conf"
#define CURSOR_DIR         "/usr/share/cursors"

// --- theme -----------------------------------------------------------

// fs_list() walks through a callback with no context pointer, so the
// index being looked for and the name found travel through these.
// Acceptable only because they are used inside theme_choice() alone,
// which is single-threaded and does not re-enter.
static int g_want_index;
static int g_seen_index;
static char g_found[SETTING_VALUE_MAX];

static void theme_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (!is_dir) return; // a theme is a DIRECTORY of shape files
    if (g_seen_index == g_want_index) k_strlcpy(g_found, name, sizeof g_found);
    g_seen_index++;
}

static int theme_choice(int index, char *out, uint32_t out_size) {
    // Enumerated from the directory rather than a compiled-in list, so
    // dropping a theme in gives it a Control Panel row with no code
    // change -- the same rule the keyboard layouts and the Start menu
    // already follow.
    g_want_index = index;
    g_seen_index = 0;
    g_found[0] = '\0';
    fs_list(CURSOR_DIR, theme_cb);
    if (!g_found[0]) return 0;
    k_strlcpy(out, g_found, out_size);
    return 1;
}

// Reads the FILE, not a live variable.
//
// The version this replaced returned the compositor's own `g_theme`,
// which is the value it has ADOPTED -- the same thing except in the
// window between a write and the compositor's next poll. Reading the
// file is what the registry means by "what is this set to", and it is
// also the only answer available to a kernel that no longer contains
// the compositor.
static void theme_get(char *out, uint32_t out_size) {
    if (!etc_config_get(CURSOR_CONFIG_FILE, "cursor_theme", out, out_size)) {
        k_strlcpy(out, "default", out_size);
    }
}

// --- size ------------------------------------------------------------

static const char *const g_sizes[] = { "normal", "large", "huge" };

static int size_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_sizes / sizeof g_sizes[0])) return 0;
    k_strlcpy(out, g_sizes[index], out_size);
    return 1;
}

static void size_get(char *out, uint32_t out_size) {
    if (!etc_config_get(CURSOR_CONFIG_FILE, "cursor_size", out, out_size)) {
        k_strlcpy(out, g_sizes[0], out_size);
    }
}

// --- the descriptors --------------------------------------------------

static const struct setting g_theme_setting = {
    .name = "cursor_theme",
    .label = "Cursor theme",
    .type = SETTING_TYPE_ENUM,
    .file = CURSOR_CONFIG_FILE,
    .choice = theme_choice,
    .get = theme_get,
    .apply = 0, // persist-only -- the compositor's poll applies it
};

static const struct setting g_size_setting = {
    .name = "cursor_size",
    .label = "Cursor size",
    .type = SETTING_TYPE_ENUM,
    .file = CURSOR_CONFIG_FILE,
    .choice = size_choice,
    .get = size_get,
    .apply = 0,
};

void cursor_theme_setting_register(void) {
    setting_register(&g_theme_setting);
    setting_register(&g_size_setting);
}
