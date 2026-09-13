// system.shell -- WHICH SHELL an interactive session starts.
//
// Four places named /bin/tosh outright: `/bin/telnetd`, the GUI
// Terminal, tolibc's `system()`, and the `tosh` service descriptor. The
// first three ask here now; the descriptor deliberately does NOT, and
// the reason is below.
//
// **THE CONSOLE SHELL IS NOT THIS SETTING, ON PURPOSE.** A service
// descriptor's `Exec=` is a data file init reads, and pointing it at a
// setting would make the one shell that is always there depend on a
// value a user can set to anything -- including a path that does not
// exist, which is a machine that boots to nothing with no prompt to fix
// it from. Linux keeps the same split: /etc/passwd names a login shell
// and init's own console is configured separately. So this changes what
// a TERMINAL gives you, and `rescue` plus the console keep working
// whatever it says.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "fs.h"

#define SHELL_CONFIG_FILE "/etc/toyos.conf"
#define SHELL_DEFAULT     "/bin/tosh"

void shell_setting_get(char *out, uint32_t cap) {
    if (!etc_config_get(SHELL_CONFIG_FILE, "shell", out, cap))
        k_strlcpy(out, SHELL_DEFAULT, cap);
}

static void shell_get(char *out, uint32_t cap) { shell_setting_get(out, cap); }

static int shell_apply(const char *value) {
    // **IT HAS TO EXIST, and that check is the whole point of refusing
    // anything.** The failure this avoids is silent and awful: a
    // Terminal window that opens and closes again with no output,
    // because the shell it was told to run is not there. A path is
    // cheap to check and the answer cannot go stale in a way that
    // matters -- a shell deleted later fails at spawn, which is
    // visible, rather than at every window for ever.
    if (!value[0]) return SETTING_INVALID;
    // The /etc parser is key=value to end of line, so whitespace would
    // be stored and read back as a different string than was typed.
    for (const char *p = value; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n') return SETTING_INVALID;
    if (value[0] != '/') return SETTING_INVALID;   // an absolute path, not a name
    if (!fs_exists(value)) return SETTING_INVALID;
    return etc_config_set(SHELL_CONFIG_FILE, "shell", value) ? SETTING_SAVED
                                                             : SETTING_UNSAVED;
}

static const struct setting g_shell_setting = {
    .name = "shell",
    .label = "Interactive shell",
    .type = SETTING_TYPE_STRING,
    .file = SHELL_CONFIG_FILE,
    .category = "System",
    .group = "Shell",
    .get = shell_get,
    .apply = shell_apply,
};

void shell_setting_register(void) {
    setting_register(&g_shell_setting);
}
