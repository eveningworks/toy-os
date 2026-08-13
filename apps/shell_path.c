// PATH: where the shell looks for an executable when you type a bare
// name. Fourth file in the shell's split (see shell_internal.h's top
// comment) -- it's its own concern rather than another pile in
// shell_sys.c, because three separate callers need it: dispatch()
// (shell.c) resolving a typed name, cmd_run() (shell_sys.c) doing the
// same thing explicitly, and apps/completion.c enumerating candidates
// for the first word of a line.
//
// This is deliberately SHELL state, not kernel state. `timezone` and
// `font_size` live in /etc/toyos.conf behind a small kernel-side
// tz.c/font_config.c because the kernel itself consults them; nothing
// in the kernel has any use for PATH -- it's a question about how a
// command line is interpreted, which is entirely the shell's business.
// So this reads the same shared config file through kapi.h's
// etc_config_get() and keeps the parsed result to itself. See
// CLAUDE.md's /etc convention and docs/decisions.md.
//
// Format (in /etc/toyos.conf):
//
//     PATH=/bin;/usr/bin
//
// Semicolon-separated, searched LEFT TO RIGHT with the first match
// winning, exactly like a real shell's PATH. A colon is accepted as a
// separator too, since that's what a Unix habit will type. Entries that
// don't exist (or aren't directories) are skipped silently rather than
// warned about -- the default below names /usr/bin, which doesn't exist
// on a stock disk, and a warning on every boot for a directory you
// haven't created yet would be noise, not information.
#include "shell.h"
#include "shell_internal.h"
#include "apps.h"

#define SHELL_PATH_CONF     "/etc/toyos.conf"
#define SHELL_PATH_KEY      "PATH"
#define SHELL_PATH_MAX_DIRS 8
// /tests comes LAST on purpose. The test binaries moved out of /bin
// into /tests (see docs/filesystem-layout.md) so that `ls /bin` and tab
// completion lead with the three real programs instead of fourteen
// mechanism exercises -- but `run nx_test`, `strace file_test` and
// friends are referenced throughout the changelog, docs/decisions.md
// and apps/terminal.c's allowlist, and rewriting all of those to carry
// an explicit /tests/ prefix would invalidate a lot of accurate
// history for no functional gain. Keeping /tests searchable, but after
// the real directories, preserves every one of those while still
// putting real programs first when a name appears in both.
#define SHELL_PATH_DEFAULT  "/bin;/usr/bin;/tests"

static char g_dirs[SHELL_PATH_MAX_DIRS][FS_PATH_MAX];
static int g_dir_count = 0;

// Splits `spec` on ';' or ':' into g_dirs. Empty entries (a doubled
// separator, or a trailing one) are skipped rather than treated as
// "the current directory" the way a real shell's empty PATH element
// historically is -- that behaviour is a well-known footgun and there's
// no reason to inherit it here.
static void parse_path(const char *spec) {
    g_dir_count = 0;
    int i = 0;
    while (spec[i] && g_dir_count < SHELL_PATH_MAX_DIRS) {
        while (spec[i] == ';' || spec[i] == ':' || spec[i] == ' ') i++;
        if (!spec[i]) break;

        int len = 0;
        char *dst = g_dirs[g_dir_count];
        while (spec[i] && spec[i] != ';' && spec[i] != ':' && len < FS_PATH_MAX - 1) {
            dst[len++] = spec[i++];
        }
        while (spec[i] && spec[i] != ';' && spec[i] != ':') i++; // skip an over-long entry's tail
        // Trim a trailing '/' so joining below never produces "//".
        while (len > 1 && dst[len - 1] == '/') len--;
        dst[len] = '\0';
        if (len > 0) g_dir_count++;
    }
}

void shell_path_init(void) {
    char spec[FS_PATH_MAX * 2];
    if (!etc_config_get(SHELL_PATH_CONF, SHELL_PATH_KEY, spec, sizeof(spec)) || spec[0] == '\0') {
        k_strcpy(spec, SHELL_PATH_DEFAULT);
    }
    parse_path(spec);
}

int shell_path_count(void) {
    return g_dir_count;
}

const char *shell_path_dir(int index) {
    if (index < 0 || index >= g_dir_count) return 0;
    return g_dirs[index];
}

// Joins a PATH directory and a bare name into `out` ("/bin" + "ls" ->
// "/bin/ls"). Returns 0 if the result wouldn't fit, which is treated as
// "not found here" rather than an error worth reporting -- a name that
// long can't name a real file on this filesystem either.
//
// kpath.h's k_path_join() does the work; this stays as a named wrapper
// because "doesn't fit means not found here" is a PATH-search decision,
// not something the string helper should imply.
static int join_path(const char *dir, const char *name, char *out) {
    return k_path_join(dir, name, out, FS_PATH_MAX);
}

int shell_path_find(const char *name, char *out) {
    if (!name || name[0] == '\0') return 0;

    // A name containing '/' is a path, not a PATH lookup -- "./foo" or
    // "/bin/foo" should mean exactly what it says, and searching PATH
    // for it would be wrong.
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            char resolved[FS_PATH_MAX];
            if (!resolve_path(name, resolved)) return 0;
            if (!fs_exists(resolved) || fs_is_dir(resolved)) return 0;
            k_strcpy(out, resolved);
            return 1;
        }
    }

    for (int i = 0; i < g_dir_count; i++) {
        char candidate[FS_PATH_MAX];
        if (!join_path(g_dirs[i], name, candidate)) continue;
        if (fs_exists(candidate) && !fs_is_dir(candidate)) {
            k_strcpy(out, candidate);
            return 1; // first match wins, left to right
        }
    }
    return 0;
}

// The one place that answers "run this name" for both a typed bare name
// (dispatch()) and an explicit `run <name>` -- so the two can't drift.
// Order: console apps from apps.c's registry first, then PATH. Shell
// builtins are handled by dispatch() before this is ever reached; see
// docs/decisions.md for why they win.
//
// Returns 1 if something was found and run (whatever its exit code), 0
// if the name resolved to nothing, leaving the caller to report it.
int shell_exec_name(const char *name, const char *args) {
    if (!name || name[0] == '\0') return 0;

    if (app_run(name)) {
        // A console app may have drawn over the whole screen (`gui`) --
        // refresh so the prompt comes back clean either way.
        vga_clear();
        vga_set_color(shell_fg, VGA_BLACK);
        return 1;
    }

    char bin_path[FS_PATH_MAX];
    if (!shell_path_find(name, bin_path)) return 0;

    int exit_code = elf_run_from_fs(bin_path, args);
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("Process finished. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_putc('\n');
    vga_set_color(shell_fg, VGA_BLACK);
    return 1;
}

void cmd_path(void) {
    vga_write("PATH (searched in order, first match wins):\n");
    if (g_dir_count == 0) {
        vga_write("  (empty)\n");
        return;
    }
    for (int i = 0; i < g_dir_count; i++) {
        vga_write("  ");
        vga_write(g_dirs[i]);
        if (!fs_is_dir(g_dirs[i])) vga_write("   (does not exist yet -- skipped)");
        vga_putc('\n');
    }
    vga_write("Set it with `edit " SHELL_PATH_CONF "` -- e.g. PATH=/bin;/usr/bin\n");
}
