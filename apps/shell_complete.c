// The kernel shell's completion ENVIRONMENT -- see shell_complete.h.
// The engine is kernel/lib/completion.c, shared with `/bin/tosh`; what
// lives here is the half that only makes sense at a `#` prompt: the
// builtin names, the console app registry, the shell's PATH, and the
// argument sets for commands ring 3 does not have.
//
// Adding an argument completer is a case in shell_arg_domain() plus,
// usually, three lines; it deliberately isn't a registration mechanism,
// because a table of function pointers for a dozen completers would be
// more machinery than the thing it's automating.
#include "font_faces.h" // the face list `fontface` completes against
#include "shell_complete.h"
#include "shell.h"
#include "apps.h"
#include "kapi.h"
#include "debugflags.h"
#include "tz.h"

// The names dispatch() handles ITSELF. Deliberately NOT a list of
// everything you can type: `rm`, `cat`, `touch` and friends are ring-3
// programs now, and complete_executables() below finds them by listing
// PATH -- which is also what keeps this table honest, since a name here
// that dispatch() does not handle is reported as an internal error.
const char *const COMPLETION_COMMANDS[] = {
    "append", "apps", "beep", "cd", "clear", "color",
    // `dmesg` was here until it became /bin/dmesg and this table's own
    // guard started reporting it: typing it hit "is tab-completable but
    // has no dispatch case". complete_executables() offers the real one
    // off PATH, which is the point of the split.
    "cursor", "debug", "dmatest", "edit", "fontface", "fontsize",
    "fputest", "fsformat", "gui", "help", "ktest", "history", "keyboard",
    "nano", "pwd", "rescue",
    "ring3test", "run", "schedtest", "steptest", "stress",
    "path", "write",
    0
};

int completion_is_known_command(const char *name) {
    for (int i = 0; COMPLETION_COMMANDS[i]; i++) {
        if (k_strcmp(COMPLETION_COMMANDS[i], name) == 0) return 1;
    }
    return 0;
}

// The colour names `color <name>` accepts. shell_sys.c's
// color_from_name() is the authority on what they mean; this is the
// same list as data, which is all a completer needs.
static const char *const COLOR_NAMES[] = {
    "black", "blue", "brown", "cyan", "darkgrey", "green", "lightblue",
    "lightcyan", "lightgreen", "lightgrey", "lightmagenta", "lightred",
    "magenta", "red", "white", "yellow", 0
};

static const char *const FONT_SIZES[] = {
    "10", "12", "14", "16", "18", "20", "24", "8", 0
};

// ---- per-command argument completers ----

static void complete_from_list(struct completion_collector *c,
                                const char *const *names) {
    for (int i = 0; names[i]; i++) completion_add(c, names[i]);
}

// The collector rides in fs_list()'s context -- nothing a listing
// touches may be a global (fs.h).
static void arg_list_cb(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)size; (void)is_dir;
    completion_add(ctx, name);
}

// `keyboard <layout>`: whatever layout files are actually on disk, not
// a hardcoded us/se -- the whole point of layouts being data files (see
// docs/decisions.md) is that a third one can be added without a rebuild.
static void complete_keyboard_layout(struct completion_collector *c) {
    if (fs_is_dir("/usr/share/kbs")) fs_list("/usr/share/kbs", arg_list_cb, c);
}

// What `cmd`'s argument number `arg_index` offers. COMPLETION_PATHS
// falls through to the engine's path completion; COMPLETION_FILLED
// means this function already added what it wanted; COMPLETION_NONE is
// free text, where offering a path would be actively unhelpful.
static enum completion_domain shell_arg_domain(struct completion_collector *c,
                                                const char *cmd, int arg_index) {
    if (k_strcmp(cmd, "color") == 0) { complete_from_list(c, COLOR_NAMES); return COMPLETION_FILLED; }
    if (k_strcmp(cmd, "fontsize") == 0) { complete_from_list(c, FONT_SIZES); return COMPLETION_FILLED; }
    if (k_strcmp(cmd, "cursor") == 0) {
        for (int i = 0; i < VGA_CURSOR_STYLE_COUNT; i++) completion_add(c, VGA_CURSOR_STYLE_NAMES[i]);
        return COMPLETION_FILLED;
    }
    if (k_strcmp(cmd, "keyboard") == 0) { complete_keyboard_layout(c); return COMPLETION_FILLED; }
    if (k_strcmp(cmd, "fontface") == 0) {
        // From the REGISTRY, not a list: the faces are whatever files
        // are in /usr/share/fonts, so a hand-maintained list here would
        // stop matching the moment somebody dropped a font in. `builtin`
        // is offered too, since it is a legal value and not a file.
        completion_add(c, "builtin");
        for (int i = 0; i < font_faces_count(); i++) {
            char name[FONT_FACE_NAME_LEN];
            if (font_faces_name(i, name, sizeof name)) completion_add(c, name);
        }
        return COMPLETION_FILLED;
    }
    // `run <name>` and `strace <binary>` take an executable. The engine
    // offers those in FIRST position; asking for them here would mean a
    // second PATH walk, so a path is offered instead and the /bin prefix
    // does the work.
    if (k_strcmp(cmd, "run") == 0) return COMPLETION_PATHS;
    // `strace <binary>`'s first argument is an executable, same as
    // `run`'s -- only its own arguments after that are free text.

    if (k_strcmp(cmd, "help") == 0) {
        completion_add(c, "tests");
        return COMPLETION_FILLED;
    }
    if (k_strcmp(cmd, "ata") == 0) {
        // `ata nodma on|off` -- the only subcommand.
        if (arg_index == 1) { completion_add(c, "nodma"); }
        else { completion_add(c, "off"); completion_add(c, "on"); }
        return COMPLETION_FILLED;
    }
    if (k_strcmp(cmd, "debug") == 0) {
        // First argument is a subsystem, second is on/off.
        if (arg_index == 1) {
            for (int i = 0; i < DBGFLAG_SUBSYS_COUNT; i++) completion_add(c, DBGFLAG_NAMES[i]);
        } else {
            completion_add(c, "off");
            completion_add(c, "on");
        }
        return COMPLETION_FILLED;
    }
    if (k_strcmp(cmd, "fsformat") == 0) {
        // Backend names only -- deliberately NOT completing "confirm",
        // which exists to be typed on purpose.
        completion_add(c, "tfs3");
        return COMPLETION_FILLED;
    }

    // Commands whose argument is free text, where completing a path
    // would be actively unhelpful.
    if (k_strcmp(cmd, "echo") == 0) return COMPLETION_NONE;
    if (k_strcmp(cmd, "stress") == 0 || k_strcmp(cmd, "steptest") == 0 ||
        k_strcmp(cmd, "dmatest") == 0) return COMPLETION_NONE;
    // `write`/`append` take a path FIRST, then free text -- only the
    // first argument is a path.
    if ((k_strcmp(cmd, "write") == 0 || k_strcmp(cmd, "append") == 0) && arg_index > 1) return COMPLETION_NONE;

    return COMPLETION_PATHS;
}

// ---- the environment, and the entry point ----

// The two filesystem hooks. Ring 0 reaches fs_list()/fs_is_dir()
// directly; ring 3's env calls sys_listdir(). This pair is the whole
// reason struct completion_env exists.
// The engine's callback type is shared with ring 3 and takes no
// context, so it rides in fs_list()'s context and this calls it.
typedef void (*engine_list_cb)(const char *, uint32_t, int);
static void engine_list_tramp(void *ctx, const char *name, uint32_t size, int is_dir) {
    ((engine_list_cb)ctx)(name, size, is_dir);
}

static void shell_list_dir(const char *path, engine_list_cb cb) {
    fs_list(path, engine_list_tramp, (void *)cb);
}

static int shell_is_dir(const char *path) { return fs_is_dir(path); }

static int shell_resolve(const char *in, char *out, int cap) {
    (void)cap; // shell_resolve_path() writes at most FS_PATH_MAX, which is the caller's buffer
    return shell_resolve_path(in, out);
}

// The console app registry: names that are neither builtins nor files,
// which is a ring-0-only concept.
static int shell_extra_count(void) { return app_registry_count; }
static const char *shell_extra_name(int i) { return app_registry[i].name; }

static const struct completion_env SHELL_ENV = {
    .commands    = COMPLETION_COMMANDS,
    .extra_count = shell_extra_count,
    .extra_name  = shell_extra_name,
    .path_count  = shell_path_count,
    .path_dir    = shell_path_dir,
    .is_dir      = shell_is_dir,
    .list_dir    = shell_list_dir,
    .resolve     = shell_resolve,
    .arg_domain  = shell_arg_domain,
};

int completion_run(const char *line, int cursor, struct completion_result *out) {
    return completion_run_env(&SHELL_ENV, line, cursor, out);
}
