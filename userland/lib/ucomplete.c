// tosh's completion environment -- see ucomplete.h. Everything here is
// a hook the shared engine (kernel/lib/completion.c) calls; there is no
// completion LOGIC in this file, which is the point of the split.
#include "ucomplete.h"
#include "lib/upath.h"
#include "rt/sys.h"
#include <string.h>

// tosh's builtins, and only its builtins. Every other name a first word
// can be is a program the engine finds by walking PATH.
//
// **THE SAME SIX is_builtin() DISPATCHES**, and they have to stay in
// step: a name here that tosh does not run completes to something that
// then fails, and one it runs but does not list simply never completes.
// Six is small enough that the list is the honest mechanism; the kernel
// shell's ~40 needed a drift check instead (shell_complete.h).
static const char *const TOSH_BUILTINS[] = {
    "bg", "cd", "fg", "help", "jobs", "pwd", 0
};

// A LISTING AT A TIME, because SYS_LISTDIR fills an array while the
// engine wants a callback. 20 KB of entries is far past a ring-3 frame
// budget (-Wframe-larger-than=2048), so the array is static -- and that
// is safe for the same reason the engine's own collector pointer is:
// nothing re-enters completion mid-listing.
#define UC_MAX_ENTRIES 256
static struct sys_dirent g_entries[UC_MAX_ENTRIES];

static void uc_list_dir(const char *path,
                         void (*cb)(const char *name, uint32_t size, int is_dir)) {
    int n = sys_listdir(path, g_entries, UC_MAX_ENTRIES);
    for (int i = 0; i < n; i++)
        cb(g_entries[i].name, g_entries[i].size, (int)g_entries[i].is_dir);
}

static int uc_is_dir(const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) < 0) return 0;
    return st.is_dir != 0;
}

// Resolves against the process's own current directory, which the
// KERNEL holds (docs/conventions/storage.md) -- so this asks for it
// rather than tracking one. `in` NULL means the cwd itself.
static int uc_resolve(const char *in, char *out, int cap) {
    if (in && in[0] == '/') {          // already absolute
        int i = 0;
        while (in[i] && i < cap - 1) { out[i] = in[i]; i++; }
        out[i] = '\0';
        return 1;
    }
    if (sys_getcwd(out, (unsigned long)cap) < 0) return 0;
    if (!in || !in[0]) return 1;

    int n = (int)strlen(out);
    if (n > 0 && out[n - 1] != '/' && n < cap - 1) out[n++] = '/';
    for (int i = 0; in[i] && n < cap - 1; i++) out[n++] = in[i];
    out[n] = '\0';
    return 1;
}

// The one command-specific rule. `cd` takes a directory, so offering
// files is offering something that cannot work -- bash and zsh both
// filter here, and it is the single argument completer whose absence is
// noticed immediately.
static enum completion_domain uc_arg_domain(struct completion_collector *c,
                                             const char *cmd, int arg_index) {
    (void)c; (void)arg_index;
    if (strcmp(cmd, "cd") == 0) return COMPLETION_DIRS;
    return COMPLETION_PATHS;
}

static const struct completion_env UC_ENV = {
    .commands   = TOSH_BUILTINS,
    .path_count = upath_dir_count,
    .path_dir   = upath_dir,
    .is_dir     = uc_is_dir,
    .list_dir   = uc_list_dir,
    .resolve    = uc_resolve,
    .arg_domain = uc_arg_domain,
};

const struct completion_env *ucomplete_env(void) { return &UC_ENV; }
