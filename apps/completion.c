// Tab-completion candidate generation -- see completion.h for the API
// and for why this is a pure function with no input handling in it.
//
// Structure: split the line into words, decide which "domain" the word
// under the cursor belongs to (commands, a specific command's argument
// set, or the filesystem), then feed every candidate in that domain
// through add_candidate(), which does the prefix filtering and the
// common-prefix arithmetic in one place. Adding a new argument
// completer is a case in complete_argument() plus, usually, three
// lines; it deliberately isn't a registration mechanism, because a
// table of function pointers for seven completers would be more
// machinery than the thing it's automating.
#include "completion.h"
#include "shell.h"
#include "apps.h"
#include "kapi.h"
#include "debugflags.h"
#include "tz.h"
#include "theme.h"

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
    "fputest", "fsck", "fsformat", "gui", "help", "ktest", "history", "keyboard",
    "nano", "pwd", "rescue",
    "ring3test", "run", "schedtest", "steptest", "stress",
    "path", "timezone", "write",
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

// ---- candidate collection ----

struct collector {
    struct completion_result *out;
    const char *prefix;
    uint32_t prefix_len;
};

static void collector_init(struct collector *c, struct completion_result *out, const char *prefix) {
    c->out = out;
    c->prefix = prefix;
    c->prefix_len = (uint32_t)k_strlen(prefix);
    out->count = 0;
    out->truncated = 0;
    out->insert[0] = '\0';
    out->add_space = 0;
}

// Adds a candidate if `match` starts with the collector's prefix, and
// stores `text` if it does.
//
// The two are usually the same string, but not for paths: there the
// prefix being matched is just the filename part ("ti"), while what the
// caller has to insert is the whole word ("/etc/timezones"). Comparing
// the full text against the filename prefix was the bug the first live
// test caught -- `cat /etc/ti<TAB>` silently found nothing, because
// "/etc/timezones" doesn't start with "ti".
//
// Also maintains the common prefix incrementally in out->insert over the
// STORED text: the first match seeds it, every later match trims it to
// where they still agree. collector_finish() turns that into what to
// actually type.
static void add_candidate_named(struct collector *c, const char *match, const char *text) {
    uint32_t len = (uint32_t)k_strlen(text);
    if (len >= COMPLETION_MAX_LEN) return; // can't represent it; skip rather than truncate into a wrong completion
    if (k_strncmp(match, c->prefix, (int)c->prefix_len) != 0) return;

    // A NAME OFFERED TWICE IS OFFERED ONCE. Several domains overlap by
    // design -- `ls` and `lspci` are builtin wrappers AND real programs
    // in /bin, so both the command table and the PATH walk produce them
    // -- and `l<Tab>` listed each of them twice. Two costs, not one: the
    // list looks broken, and duplicates spend slots against
    // COMPLETION_MAX_CANDIDATES, so enough of them can push a real
    // candidate out and set `truncated`.
    //
    // Linear, and deliberately: COMPLETION_MAX_CANDIDATES is 48, so the
    // worst case is a couple of thousand k_strcmp()s of short strings
    // once per Tab. A hash set would be more code than the thing it
    // speeds up.
    //
    // Note this cannot affect `insert`: the common prefix of a set is
    // unchanged by adding a member it already has.
    for (int i = 0; i < c->out->count; i++) {
        if (k_strcmp(c->out->candidates[i], text) == 0) return;
    }

    if (c->out->count == 0) {
        k_strcpy(c->out->insert, text);
    } else {
        uint32_t i = 0;
        while (c->out->insert[i] && text[i] && c->out->insert[i] == text[i]) i++;
        c->out->insert[i] = '\0';
    }

    if (c->out->count < COMPLETION_MAX_CANDIDATES) {
        k_strcpy(c->out->candidates[c->out->count], text);
        c->out->count++;
    } else {
        c->out->truncated = 1;
    }
}

// The common case: the text to insert is the thing being matched.
static void add_candidate(struct collector *c, const char *name) {
    add_candidate_named(c, name, name);
}

// Sorts the candidates in place, so a listing reads alphabetically
// rather than in the order the domains happened to produce it --
// builtin-table order, then the app registry, then raw directory order
// per PATH entry, which is what a reader was being asked to scan.
// bash and zsh both sort; an unsorted list of forty is unusable.
//
// Insertion sort on at most COMPLETION_MAX_CANDIDATES (48) short
// strings, once per Tab. The array is fixed-size rows, so this swaps
// whole rows through one scratch row rather than moving pointers --
// still nothing at this scale, and it keeps the storage a plain 2D
// array that the callers already read directly.
//
// Sorting cannot affect `insert` either: the common prefix of a set
// does not depend on the order its members arrived in.
static void sort_candidates(struct completion_result *out) {
    for (int i = 1; i < out->count; i++) {
        char key[COMPLETION_MAX_LEN];
        k_strcpy(key, out->candidates[i]);
        int j = i - 1;
        while (j >= 0 && k_strcmp(out->candidates[j], key) > 0) {
            k_strcpy(out->candidates[j + 1], out->candidates[j]);
            j--;
        }
        k_strcpy(out->candidates[j + 1], key);
    }
}

// Turns the accumulated common prefix into "what to actually insert",
// and decides whether a trailing space belongs.
static void collector_finish(struct collector *c) {
    struct completion_result *out = c->out;
    if (out->count == 0) { out->insert[0] = '\0'; return; }
    sort_candidates(out);

    uint32_t common_len = (uint32_t)k_strlen(out->insert);
    if (common_len < c->prefix_len) {
        // Can't happen -- every candidate starts with the prefix, so the
        // common prefix is at least that long. Defensive: never hand
        // back something that would delete typed text.
        out->insert[0] = '\0';
        return;
    }
    // Shift off the part the user already typed.
    uint32_t i = 0;
    while (out->insert[c->prefix_len + i]) {
        out->insert[i] = out->insert[c->prefix_len + i];
        i++;
    }
    out->insert[i] = '\0';

    // A single candidate is finished, so follow it with a space --
    // except a directory, where the user is almost certainly about to
    // type the next component.
    if (out->count == 1 && !out->truncated) {
        uint32_t clen = (uint32_t)k_strlen(out->candidates[0]);
        out->add_space = (clen > 0 && out->candidates[0][clen - 1] == '/') ? 0 : 1;
    }
}

// ---- filesystem candidates ----

// fs_list() takes a plain callback with no context pointer, so the
// in-flight collector has to be reachable from a file-scope pointer.
// Safe for the same reason every other static in apps/ is: this kernel
// is single-threaded and nothing re-enters completion mid-listing.
static struct collector *g_active_collector;
static const char *g_dir_prefix; // the part of the word before the last '/', kept so candidates come back as full words
static int g_skip_dirs;          // set while walking PATH: a directory is not a command

static void fs_list_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    // A DIRECTORY IS NEVER A COMMAND. Walking PATH used to offer
    // /bin/wm as the candidate `wm/`, which cannot be a first word --
    // shell_path_find() explicitly refuses a directory. Costs nothing
    // to filter: `is_dir` is already an argument here, so the "a second
    // fs_is_dir() call per entry" this once traded away was never the
    // price. Path completion still lists directories, which is the
    // whole point there.
    if (g_skip_dirs && is_dir) return;
    // `named` (with the trailing '/' for a directory) is what the
    // filename prefix is matched against; `full` prepends the directory
    // part the user already typed, and is what gets inserted.
    char named[COMPLETION_MAX_LEN];
    char full[COMPLETION_MAX_LEN];
    uint32_t n = 0;
    for (const char *p = name; *p && n < COMPLETION_MAX_LEN - 2; p++) named[n++] = *p;
    if (is_dir && n < COMPLETION_MAX_LEN - 2) named[n++] = '/'; // so the next Tab descends
    named[n] = '\0';

    uint32_t pos = 0;
    for (const char *p = g_dir_prefix; *p && pos < COMPLETION_MAX_LEN - 2; p++) full[pos++] = *p;
    for (const char *p = named; *p && pos < COMPLETION_MAX_LEN - 1; p++) full[pos++] = *p;
    full[pos] = '\0';

    add_candidate_named(g_active_collector, named, full);
}

// Completes `word` as a path. The word is split at its last '/': the
// part before it names the directory to list (resolved against the
// shell's cwd), the part after it is the prefix to match. Candidates
// come back as whole words (directory part included) so the caller can
// treat them uniformly with every other domain.
static void complete_path(struct collector *c, const char *word) {
    char dir_part[FS_PATH_MAX];
    char resolved[FS_PATH_MAX];
    static char dir_prefix[COMPLETION_MAX_LEN];

    int last_slash = -1;
    for (int i = 0; word[i]; i++) if (word[i] == '/') last_slash = i;

    if (last_slash < 0) {
        dir_part[0] = '\0';       // no '/' at all -- list the cwd
        dir_prefix[0] = '\0';
    } else {
        int dlen = last_slash == 0 ? 1 : last_slash; // keep the leading '/' for a root-relative word
        if (dlen >= (int)sizeof(dir_part)) return;
        k_memcpy(dir_part, word, (uint32_t)dlen);
        dir_part[dlen] = '\0';
        if (last_slash + 1 >= (int)sizeof(dir_prefix)) return;
        k_memcpy(dir_prefix, word, (uint32_t)last_slash + 1);
        dir_prefix[last_slash + 1] = '\0';
    }

    if (!shell_resolve_path(dir_part[0] ? dir_part : 0, resolved)) return;
    if (!fs_is_dir(resolved)) return;

    // Re-point the collector's prefix at the filename part only; the
    // callback prepends dir_prefix so candidates stay whole words.
    const char *file_prefix = last_slash < 0 ? word : word + last_slash + 1;
    struct collector inner = *c;
    inner.prefix = file_prefix;
    inner.prefix_len = (uint32_t)k_strlen(file_prefix);

    g_active_collector = &inner;
    g_dir_prefix = dir_prefix;
    fs_list(resolved, fs_list_cb);
    g_active_collector = 0;

    // add_candidate() matched against the filename part, but the caller
    // is replacing the whole word -- so the prefix the common-prefix
    // arithmetic must subtract is the whole word, not just the filename.
    *c = inner;
    c->prefix = word;
    c->prefix_len = (uint32_t)k_strlen(word);
}

// ---- per-command argument completers ----

static void complete_from_list(struct collector *c, const char *const *names) {
    for (int i = 0; names[i]; i++) add_candidate(c, names[i]);
}

// Everything a bare name can resolve to: console apps from apps.c's
// registry, then every executable in every PATH directory. Used both for
// `run <name>` and -- since the `run` prefix became optional -- for the
// first word of a line, alongside the builtin command names.
//
// Directories inside a PATH entry are SKIPPED (g_skip_dirs): a
// directory cannot be a first word, and shell_path_find() refuses one
// explicitly, so offering /bin/wm as `wm/` was offering something that
// could never run.
//
// A name appearing in more than one PATH directory, or in both the app
// registry and /bin, is offered once -- add_candidate_named() dedupes.
// That is not the same as PATH order becoming irrelevant: the FIRST
// match still wins at run time (shell_path.c), and completion is only
// saying the name exists.
static void complete_executables(struct collector *c) {
    for (int i = 0; i < app_registry_count; i++) add_candidate(c, app_registry[i].name);

    static char empty[1] = "";
    struct collector inner = *c;
    g_active_collector = &inner;
    g_dir_prefix = empty;
    g_skip_dirs = 1;
    for (int i = 0; i < shell_path_count(); i++) {
        const char *dir = shell_path_dir(i);
        if (dir && fs_is_dir(dir)) fs_list(dir, fs_list_cb);
    }
    g_skip_dirs = 0;
    g_active_collector = 0;
    *c = inner;
}

// `keyboard <layout>`: whatever layout files are actually on disk, not
// a hardcoded us/se -- the whole point of layouts being data files (see
// docs/decisions.md) is that a third one can be added without a rebuild.
static void complete_keyboard_layout(struct collector *c) {
    static char empty[1] = "";
    struct collector inner = *c;
    g_active_collector = &inner;
    g_dir_prefix = empty;
    if (fs_is_dir("/etc/kbs")) fs_list("/etc/kbs", fs_list_cb);
    g_active_collector = 0;
    *c = inner;
}

// Returns 1 if `cmd` has a known argument set (and fills the collector),
// 0 if its arguments are paths (or anything else) and the caller should
// fall through to path completion.
static int complete_argument(struct collector *c, const char *cmd, int arg_index) {
    if (k_strcmp(cmd, "color") == 0) { complete_from_list(c, COLOR_NAMES); return 1; }
    if (k_strcmp(cmd, "fontsize") == 0) { complete_from_list(c, FONT_SIZES); return 1; }
    if (k_strcmp(cmd, "cursor") == 0) {
        for (int i = 0; i < VGA_CURSOR_STYLE_COUNT; i++) add_candidate(c, VGA_CURSOR_STYLE_NAMES[i]);
        return 1;
    }
    if (k_strcmp(cmd, "keyboard") == 0) { complete_keyboard_layout(c); return 1; }
    if (k_strcmp(cmd, "fontface") == 0) {
        // From the REGISTRY, not a list: the faces are whatever files
        // are in /usr/share/fonts, so a hand-maintained list here would
        // stop matching the moment somebody dropped a font in. `builtin`
        // is offered too, since it is a legal value and not a file.
        add_candidate(c, "builtin");
        for (int i = 0; i < font_face_count(); i++) {
            struct font_face_info info;
            if (font_face_info(i, &info)) add_candidate(c, info.name);
        }
        return 1;
    }
    if (k_strcmp(cmd, "run") == 0) { complete_executables(c); return 1; }
    // `strace <binary>`'s first argument is an executable, same as
    // `run`'s -- only its own arguments after that are free text.

    if (k_strcmp(cmd, "help") == 0) {
        add_candidate(c, "tests");
        return 1;
    }
    if (k_strcmp(cmd, "timezone") == 0) {
        int n = tz_city_count();
        for (int i = 0; i < n; i++) add_candidate(c, tz_city_name(i));
        return 1;
    }
    if (k_strcmp(cmd, "ata") == 0) {
        // `ata nodma on|off` -- the only subcommand.
        if (arg_index == 1) { add_candidate(c, "nodma"); }
        else { add_candidate(c, "off"); add_candidate(c, "on"); }
        return 1;
    }
    if (k_strcmp(cmd, "debug") == 0) {
        // First argument is a subsystem, second is on/off.
        if (arg_index == 1) {
            for (int i = 0; i < DBGFLAG_SUBSYS_COUNT; i++) add_candidate(c, DBGFLAG_NAMES[i]);
        } else {
            add_candidate(c, "off");
            add_candidate(c, "on");
        }
        return 1;
    }
    if (k_strcmp(cmd, "fsck") == 0) { add_candidate(c, "repair"); return 1; }
    if (k_strcmp(cmd, "fsformat") == 0) {
        // Backend names only -- deliberately NOT completing "confirm",
        // which exists to be typed on purpose.
        add_candidate(c, "tfs3");
        return 1;
    }

    // Commands whose argument is free text, where completing a path
    // would be actively unhelpful.
    if (k_strcmp(cmd, "echo") == 0) return 1;
    if (k_strcmp(cmd, "stress") == 0 || k_strcmp(cmd, "steptest") == 0 ||
        k_strcmp(cmd, "dmatest") == 0) return 1;
    // `write`/`append` take a path FIRST, then free text -- only the
    // first argument is a path.
    if ((k_strcmp(cmd, "write") == 0 || k_strcmp(cmd, "append") == 0) && arg_index > 1) return 1;

    return 0; // fall through to path completion
}

// ---- entry point ----

int completion_run(const char *line, int cursor, struct completion_result *out) {
    struct collector c;

    if (cursor < 0) cursor = 0;
    // Find the word under the cursor: back up to the previous space.
    int start = cursor;
    while (start > 0 && line[start - 1] != ' ') start--;

    static char word[COMPLETION_MAX_LEN];
    int wlen = cursor - start;
    if (wlen >= COMPLETION_MAX_LEN) { // absurdly long word -- nothing sensible to offer
        collector_init(&c, out, "");
        out->count = 0;
        return 0;
    }
    k_memcpy(word, line + start, (uint32_t)wlen);
    word[wlen] = '\0';

    // Which argument is this? Count the words before it, skipping runs
    // of spaces so "cat   /et<TAB>" behaves like "cat /et<TAB>".
    static char cmd[COMPLETION_MAX_LEN];
    cmd[0] = '\0';
    int arg_index = 0;
    {
        int i = 0, words = 0;
        while (i < start) {
            while (i < start && line[i] == ' ') i++;
            if (i >= start) break;
            int wstart = i;
            while (i < start && line[i] != ' ') i++;
            if (words == 0) {
                int len = i - wstart;
                if (len < COMPLETION_MAX_LEN) {
                    k_memcpy(cmd, line + wstart, (uint32_t)len);
                    cmd[len] = '\0';
                }
            }
            words++;
        }
        arg_index = words; // 0 = the command itself, 1 = first argument, ...
    }

    collector_init(&c, out, word);

    if (arg_index == 0) {
        // Builtins first, then everything a bare name can also resolve
        // to now that `run` is optional -- registry apps and PATH
        // executables. Same order dispatch() actually tries them in.
        complete_from_list(&c, COMPLETION_COMMANDS);
        complete_executables(&c);
    } else if (!complete_argument(&c, cmd, arg_index)) {
        complete_path(&c, word);
    }

    collector_finish(&c);
    return out->count;
}
