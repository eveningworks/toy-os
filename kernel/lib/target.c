// See target.h. The /etc plumbing for the boot target, in the same
// shape as font_config.c and cursor_config.c -- read one key at boot,
// write it back on change -- plus the one thing those do not have: a
// kernel command line that can override the value for a single boot.
#include "target.h"
#include "etc_config.h"
#include "setting.h"
#include "multiboot.h"
#include "string.h"

#define TARGET_FILE "/etc/toyos.conf"
#define TARGET_KEY  "default_target"

// Registry-choice order, and the order `config` lists them in.
static const char *const TARGET_NAMES[] = { TARGET_TEXT, TARGET_GRAPHICAL };
#define TARGET_COUNT ((int)(sizeof TARGET_NAMES / sizeof TARGET_NAMES[0]))

// Graphical by default: this OS boots to a desktop, and a machine whose
// /etc has never been written should come up the way the ISO advertises.
static char g_target[16] = TARGET_GRAPHICAL;
static int g_overridden;

static int target_valid(const char *name) {
    for (int i = 0; i < TARGET_COUNT; i++)
        if (k_strcmp(name, TARGET_NAMES[i]) == 0) return 1;
    return 0;
}

// Finds `target=<word>` on the command line.
//
// Matching is by substring like every other boot word here, with ONE
// extra condition: the match must start the line or follow a space.
// Without it `default_target=graphical` -- which is what this key is
// called everywhere else, and therefore exactly what somebody will
// eventually type -- contains `target=` and would be read as the
// override. A boot word that silently matches a different, longer word
// is the kind of bug that is only ever found by the person it bites.
static int cmdline_target(char *out, uint32_t out_size) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;

    for (const char *p = cmdline; (p = k_strstr(p, "target=")) != 0; p += 7) {
        if (p != cmdline && p[-1] != ' ') continue;
        const char *v = p + 7;
        uint32_t n = 0;
        while (v[n] && v[n] != ' ' && n + 1 < out_size) n++;
        if (n == 0) return 0;
        k_memcpy(out, v, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

void target_init(void) {
    char value[16];

    if (etc_config_get(TARGET_FILE, TARGET_KEY, value, sizeof value)
        && target_valid(value))
        k_strlcpy(g_target, value, sizeof g_target);

    // The override goes over the top and is NOT written back -- see
    // target.h. An unrecognised name is ignored rather than fatal,
    // matching every other /etc reader here: a typo on the GRUB line
    // should not be the thing that stops a machine booting.
    if (cmdline_target(value, sizeof value) && target_valid(value)) {
        k_strlcpy(g_target, value, sizeof g_target);
        g_overridden = 1;
    }
}

const char *target_get(void) { return g_target; }
int target_overridden(void) { return g_overridden; }

// --- the registry descriptor (see setting.h) -------------------------

static int target_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= TARGET_COUNT) return 0;
    k_strlcpy(out, TARGET_NAMES[index], out_size);
    return 1;
}

static void target_setting_get(char *out, uint32_t out_size) {
    k_strlcpy(out, g_target, out_size);
}

// Applying it changes what the NEXT boot starts, never what is running:
// switching target does not tear down a live desktop, and a desktop is
// not started by writing the key. That is deliberate -- init owns what
// runs, and a setting that reached across into another process's
// lifetime would be a second, invisible way to kill the desktop.
static int target_apply(const char *value) {
    if (!target_valid(value)) return SETTING_INVALID;
    k_strlcpy(g_target, value, sizeof g_target);
    g_overridden = 0; // the file and the live value agree again
    return etc_config_set(TARGET_FILE, TARGET_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_target_setting = {
    .name   = TARGET_KEY,
    .label  = "Startup target",
    .type   = SETTING_TYPE_ENUM,
    .file   = TARGET_FILE,
    .category = "Startup",
    .choice = target_choice,
    .get    = target_setting_get,
    .apply  = target_apply,
};

void target_setting_register(void) { setting_register(&g_target_setting); }
