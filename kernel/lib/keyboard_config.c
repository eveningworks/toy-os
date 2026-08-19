// Persists the keyboard layout selection across reboots -- the same
// "small key=value entry in the shared /etc/toyos.conf, read once at
// boot" pattern tz.c and font_config.c already use (see etc_config.h).
// No legacy pre-toyos.conf file to migrate from here (unlike
// tz.c/font_config.c) -- this setting was added after toyos.conf
// already existed, so it never had an earlier standalone-file form.
#include "keyboard_config.h"
#include "keyboard_layout.h"
#include "fs.h"
#include "string.h"
#include "etc_config.h"
#include "setting.h"
#include "multiboot.h"

#define KEYBOARD_CONFIG_FILE "/etc/toyos.conf"
#define KEYBOARD_CONFIG_KEY "keyboard_layout"

// Finds `kbd=<name>` on the kernel command line -- the layout override
// for ONE boot, in the same shape as target.c's `target=` and for a
// related reason: a test that types punctuation over QMP needs a known
// layout, because a QMP qcode names a PHYSICAL KEY by its US label and
// the character it produces is whatever the GUEST's layout says. Under
// the `se` default that made `/` arrive as `-`, so `spawn /bin/tosh`
// became `spawn -bin-tosh` -- and a substring assertion passed anyway.
// See tools/shell_flow.py's table comment.
//
// NOT written back to /etc, exactly like `target=`: an override is for
// this boot, and a test harness must not silently reconfigure the
// machine it borrowed. `config diff` shows it as a live-vs-stored
// difference.
//
// The match must start the line or follow a space, so a longer word
// ending in `kbd=` cannot be read as this one -- target.c's own trap,
// stated once there and worth not re-learning.
static int cmdline_layout(char *out, uint32_t out_size) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;

    for (const char *p = cmdline; (p = k_strstr(p, "kbd=")) != 0; p += 4) {
        if (p != cmdline && p[-1] != ' ') continue;
        const char *v = p + 4;
        uint32_t n = 0;
        while (v[n] && v[n] != ' ' && n + 1 < out_size) n++;
        if (n == 0) return 0;
        k_memcpy(out, v, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

void keyboard_config_init(void) {
    char value[KB_LAYOUT_NAME_MAX];

    // The override wins over the persisted value, and a name that has
    // no table is IGNORED rather than fatal -- keyboard_layout_load()
    // reports that, and a typo on the GRUB line must not be what stops
    // a machine booting or leaves it with no keyboard at all.
    if (cmdline_layout(value, sizeof value) && keyboard_layout_load(value)) return;

    // Always call keyboard_layout_load() -- even with no persisted
    // value -- so the layout tables are actually populated by the
    // time this returns. keyboard_layout_load()'s own fallback chain
    // (requested name -> /etc/kbs/us -> compiled-in US) means passing
    // "us" here when nothing's persisted yet does exactly the right
    // thing either way.
    if (!etc_config_get(KEYBOARD_CONFIG_FILE, KEYBOARD_CONFIG_KEY, value, sizeof(value))) {
        keyboard_layout_load("us");
        return;
    }
    keyboard_layout_load(value);
}

int keyboard_config_save(const char *name) {
    if (!name || !*name) return SETTING_INVALID;
    return etc_config_set(KEYBOARD_CONFIG_FILE, KEYBOARD_CONFIG_KEY, name)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// --- the registry descriptor (see setting.h) -------------------------
//
// This is the setting the registry's `choice` CALLBACK exists for: the
// layouts are not a compiled-in enum, they are whatever files sit in
// /etc/kbs (tools/gen_kbs.py puts them there), so the option list has
// to be read off the disk at the moment it is asked for. An array
// would have to be rebuilt every time a layout file appeared.
//
// The trap: fs_list() takes a bare callback with no user pointer, so
// the walk's state is file-static. That is sound only because this
// kernel is single-threaded and the state never outlives the one
// fs_list() call below -- do not hold it across anything that yields.

#define KB_LAYOUT_DIR "/etc/kbs"

static int g_walk_want;   // which index the caller asked for
static int g_walk_seen;   // how many entries the walk has passed
static char g_walk_name[KB_LAYOUT_NAME_MAX];
static int g_walk_found;

static void kb_walk(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (is_dir || g_walk_found) return;
    if (g_walk_seen++ != g_walk_want) return;
    k_strlcpy(g_walk_name, name, sizeof g_walk_name);
    g_walk_found = 1;
}

static int kb_choice(int index, char *out, uint32_t out_size) {
    if (index < 0) return 0;
    g_walk_want = index;
    g_walk_seen = 0;
    g_walk_found = 0;
    g_walk_name[0] = '\0';
    fs_list(KB_LAYOUT_DIR, kb_walk);
    if (!g_walk_found) return 0;
    k_strlcpy(out, g_walk_name, out_size);
    return 1;
}

static void kb_get(char *out, uint32_t out_size) {
    k_strlcpy(out, keyboard_layout_current(), out_size);
}

static int kb_apply(const char *value) {
    if (!value || !*value) return SETTING_INVALID;
    // keyboard_layout_load() has its own fallback chain, so a bad name
    // leaves a WORKING keyboard rather than none -- but it would also
    // make this report success for a layout that isn't the one asked
    // for. Check the name took before persisting it.
    if (!keyboard_layout_load(value)) return SETTING_INVALID;
    if (k_strcmp(keyboard_layout_current(), value) != 0) return SETTING_INVALID;
    return keyboard_config_save(value);
}

static const struct setting g_kb_setting = {
    .name   = KEYBOARD_CONFIG_KEY,
    .label  = "Keyboard layout",
    .type   = SETTING_TYPE_ENUM,
    .file   = KEYBOARD_CONFIG_FILE,
    .choice = kb_choice,
    .get    = kb_get,
    .apply  = kb_apply,
};

void keyboard_config_setting_register(void) { setting_register(&g_kb_setting); }
