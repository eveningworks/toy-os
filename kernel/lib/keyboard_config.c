// The keyboard's three settings -- the layout list, the active layout
// and the dead-keys switch -- in the shared /etc/toyos.conf, read once at
// boot: the pattern tz.c and font_config.c use (see etc_config.h).
#include "keyboard_config.h"
#include "keyboard_layout.h"
#include "fs.h"
#include "string.h"
#include "etc_config.h"
#include "setting.h"
#include "multiboot.h"
#include "initcall.h"

#define KEYBOARD_CONFIG_FILE "/etc/toyos.conf"
#define KEYBOARD_CONFIG_KEY "keyboard_layout"
#define KEYBOARD_LAYOUTS_KEY "keyboard_layouts"
#define KEYBOARD_DEADKEYS_KEY "keyboard_dead_keys"
#define KB_LAYOUTS_MAX 8

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

// THE LIST, `keyboard_layouts=fi,us,de`: every layout Super+Space and
// the taskbar indicator switch between, the FIRST being the one a boot
// starts with. Held here as the live copy; keyboard_layout_current() is
// which of them is active, and switching does not touch the file.
static char g_layouts[SETTING_VALUE_MAX];

// Is `name` one of the comma-separated entries of `list`?
static int list_has(const char *list, const char *name) {
    uint32_t n = k_strlen(name);
    for (const char *p = list; *p; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        if ((uint32_t)(e - p) == n && k_strncmp(p, name, n) == 0) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

// The first entry of `list` into `out`.
static void list_first(const char *list, char *out, uint32_t out_size) {
    uint32_t n = 0;
    while (list[n] && list[n] != ',' && n + 1 < out_size) { out[n] = list[n]; n++; }
    out[n] = '\0';
}

static int dead_keys_from(const char *v) { return !v || k_strcmp(v, "off") != 0; }

void keyboard_config_init(void) {
    char value[KB_LAYOUT_NAME_MAX];
    char dk[8];
    keyboard_layout_set_dead_keys(
        dead_keys_from(etc_config_get(KEYBOARD_CONFIG_FILE, KEYBOARD_DEADKEYS_KEY, dk, sizeof dk)
                           ? dk : 0));

    // The list, or -- on a machine configured before there was one --
    // the single layout it persisted, as a list of one.
    if (!etc_config_get(KEYBOARD_CONFIG_FILE, KEYBOARD_LAYOUTS_KEY, g_layouts, sizeof g_layouts) ||
        !g_layouts[0]) {
        if (!etc_config_get(KEYBOARD_CONFIG_FILE, KEYBOARD_CONFIG_KEY, g_layouts, sizeof g_layouts) ||
            !g_layouts[0])
            k_strlcpy(g_layouts, "us", sizeof g_layouts);
    }

    // The override wins over the persisted value, and a name that has
    // no table is IGNORED rather than fatal -- keyboard_layout_load()
    // reports that, and a typo on the GRUB line must not be what stops
    // a machine booting or leaves it with no keyboard at all.
    if (cmdline_layout(value, sizeof value) && keyboard_layout_load(value)) return;

    // Always call keyboard_layout_load(), so the tables are populated by
    // the time this returns; its own fallback chain (requested name ->
    // /usr/share/kbs/us -> compiled-in US) covers a bad first entry.
    list_first(g_layouts, value, sizeof value);
    keyboard_layout_load(value);
}
INITCALL(keyboard_config_init, INIT_CONFIG);

const char *keyboard_config_layouts(void) { return g_layouts; }

// The in-kernel shell's `keyboard <name>`: that layout becomes the one a
// boot starts with -- moved to the front of the list, or put there.
int keyboard_config_save(const char *name) {
    if (!name || !*name || k_strlen(name) >= KB_LAYOUT_NAME_MAX) return SETTING_INVALID;
    char list[SETTING_VALUE_MAX];
    k_strlcpy(list, name, sizeof list);
    for (const char *p = g_layouts; *p; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        uint32_t n = (uint32_t)(e - p);
        if (n && !(n == k_strlen(name) && k_strncmp(p, name, n) == 0) &&
            k_strlen(list) + 1 + n < sizeof list) {
            k_strlcat(list, ",", sizeof list);
            uint32_t at = k_strlen(list);
            k_memcpy(list + at, p, n);
            list[at + n] = '\0';
        }
        p = *e ? e + 1 : e;
    }
    k_strlcpy(g_layouts, list, sizeof g_layouts);
    return etc_config_set(KEYBOARD_CONFIG_FILE, KEYBOARD_LAYOUTS_KEY, list)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// --- the registry descriptor (see setting.h) -------------------------
//
// This is the setting the registry's `choice` CALLBACK exists for: the
// layouts are not a compiled-in enum, they are whatever files sit in
// /usr/share/kbs (tools/gen_kbs.py puts them there), so the option list has
// to be read off the disk at the moment it is asked for. An array
// would have to be rebuilt every time a layout file appeared.
//
// The walk's state is the CALL's, passed through fs_list()'s context --
// it used to be file-static on the grounds that nothing yields, and the
// walk itself does (fs.h's fs_list()).

#define KB_LAYOUT_DIR "/usr/share/kbs"

struct kb_walk_ctx {
    int want;    // which index the caller asked for
    int seen;    // how many entries the walk has passed
    int found;
    char name[KB_LAYOUT_NAME_MAX];
};

static void kb_walk(void *ctx, const char *name, uint32_t size, int is_dir) {
    struct kb_walk_ctx *w = ctx;
    (void)size;
    if (is_dir || w->found) return;
    if (w->seen++ != w->want) return;
    k_strlcpy(w->name, name, sizeof w->name);
    w->found = 1;
}

static int kb_choice(int index, char *out, uint32_t out_size) {
    if (index < 0) return 0;
    struct kb_walk_ctx w = { .want = index };
    fs_list(KB_LAYOUT_DIR, kb_walk, &w);
    if (!w.found) return 0;
    k_strlcpy(out, w.name, out_size);
    return 1;
}

static void kb_get(char *out, uint32_t out_size) {
    k_strlcpy(out, keyboard_layout_current(), out_size);
}

// THE ACTIVE LAYOUT, which Super+Space and the taskbar indicator set.
// Live only: what a boot starts with is the list's first entry, so a
// switch is never written back -- Windows' and KDE's default too.
static int kb_apply(const char *value) {
    if (!value || !*value) return SETTING_INVALID;
    // keyboard_layout_load() has its own fallback chain, so a bad name
    // leaves a WORKING keyboard rather than none -- but it would also
    // make this report success for a layout that isn't the one asked
    // for. Check the name took.
    if (!keyboard_layout_load(value)) return SETTING_INVALID;
    if (k_strcmp(keyboard_layout_current(), value) != 0) return SETTING_INVALID;
    return SETTING_SAVED;
}

static const struct setting g_kb_setting = {
    .name   = KEYBOARD_CONFIG_KEY,
    .label  = "Active layout",
    .type   = SETTING_TYPE_ENUM,
    .file   = KEYBOARD_CONFIG_FILE,
    .category = "Input",
    .group = "Keyboard",
    .choice = kb_choice,
    .get    = kb_get,
    .apply  = kb_apply,
};

// --- the list ----------------------------------------------------------

static void kbl_get(char *out, uint32_t out_size) { k_strlcpy(out, g_layouts, out_size); }

// A REJECTED LIST CHANGES NOTHING: every entry must name a layout file,
// once, and there must be one to eight of them.
static int kbl_valid(const char *value) {
    int count = 0;
    for (const char *p = value; ; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        uint32_t n = (uint32_t)(e - p);
        if (n == 0 || n >= KB_LAYOUT_NAME_MAX || ++count > KB_LAYOUTS_MAX) return 0;
        char name[KB_LAYOUT_NAME_MAX], path[48];
        k_memcpy(name, p, n);
        name[n] = '\0';
        k_strlcpy(path, KB_LAYOUT_DIR "/", sizeof path);
        k_strlcat(path, name, sizeof path);
        if (!fs_exists(path)) return 0;
        // A duplicate: the same name earlier in the list.
        for (const char *q = value; q < p; ) {
            const char *qe = q;
            while (*qe != ',') qe++;
            if ((uint32_t)(qe - q) == n && k_strncmp(q, name, n) == 0) return 0;
            q = qe + 1;
        }
        if (!*e) return 1;
        p = e + 1;
    }
}

static int kbl_apply(const char *value) {
    if (!value || !*value || !kbl_valid(value)) return SETTING_INVALID;
    k_strlcpy(g_layouts, value, sizeof g_layouts);
    // A list that no longer holds the active layout switches to its
    // first -- the layout a boot would start with.
    if (!list_has(g_layouts, keyboard_layout_current())) {
        char first[KB_LAYOUT_NAME_MAX];
        list_first(g_layouts, first, sizeof first);
        keyboard_layout_load(first);
    }
    return etc_config_set(KEYBOARD_CONFIG_FILE, KEYBOARD_LAYOUTS_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_kbl_setting = {
    .name   = KEYBOARD_LAYOUTS_KEY,
    .label  = "Layouts",
    .type   = SETTING_TYPE_STRING,
    .file   = KEYBOARD_CONFIG_FILE,
    .category = "Input",
    .group = "Keyboard",
    .get    = kbl_get,
    .apply  = kbl_apply,
};

// --- dead keys ---------------------------------------------------------

static const char *const g_onoff[] = { "on", "off" };
static int dk_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index > 1) return 0;
    k_strlcpy(out, g_onoff[index], out_size);
    return 1;
}
static void dk_get(char *out, uint32_t out_size) {
    k_strlcpy(out, keyboard_layout_dead_keys() ? "on" : "off", out_size);
}
static int dk_apply(const char *value) {
    if (!value || (k_strcmp(value, "on") != 0 && k_strcmp(value, "off") != 0))
        return SETTING_INVALID;
    keyboard_layout_set_dead_keys(dead_keys_from(value));
    return etc_config_set(KEYBOARD_CONFIG_FILE, KEYBOARD_DEADKEYS_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_dk_setting = {
    .name   = KEYBOARD_DEADKEYS_KEY,
    .label  = "Dead keys",
    .type   = SETTING_TYPE_ENUM,
    .file   = KEYBOARD_CONFIG_FILE,
    .category = "Input",
    .group = "Keyboard",
    .choice = dk_choice,
    .get    = dk_get,
    .apply  = dk_apply,
};

void keyboard_config_setting_register(void) {
    setting_register(&g_kbl_setting);
    setting_register(&g_kb_setting);
    setting_register(&g_dk_setting);
}
