// `config` -- read, change and find this machine's configuration.
//
// toy-os keeps its settings the way Unix does: plain `name=value` text
// under /etc, editable with `edit` and readable with `cat`. That is
// deliberate and is not changing. What it costs is the thing everyone
// who has used /etc knows -- finding the right file, and knowing which
// keys a file is even allowed to contain. Nothing in a directory of
// text files can answer either question about itself.
//
// So the kernel keeps two registries and this program is their front
// end (api/setting.h, api/config_file.h, over SYS_SETTING):
//
//   * SETTINGS -- typed, validated, applied. `list` shows every one
//     WITH THE FILE IT LIVES IN, which is the index /etc never had.
//   * CONFIG FILES -- the /etc documents themselves, including ones
//     holding no registered setting at all (the timezone database, the
//     keymap tables). `files` is the map; `register` adds to it.
//
// THE DESIGN POINT WORTH KEEPING: this program contains no `name=value`
// parser. Every value it reports comes back from the kernel's one
// parser (kernel/lib/etc_config.c), including the FILE's value as
// distinct from the live one (`diff`). A second parser here would drift
// from that one, and the drift would show up as a setting that `config`
// and the system disagree about -- which is precisely the class of bug
// the shared kernel/lib toolkit exists to prevent. `find` searches raw
// LINES for that reason: grep semantics need no parser at all.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "setting_abi.h" // enum setting_result, struct setting_msg

static void put(const char *s) { sys_print(s); }

static void putline(const char *s) { sys_print(s); sys_print("\n"); }

// --- talking to the registry -----------------------------------------

static int op(struct setting_msg *m, uint32_t which) {
    memset(m, 0, sizeof *m);
    m->op = which;
    return sys_setting(m) == 0;
}

// Every path that changes a setting goes through this, so the
// three-way outcome is reported the same way everywhere. SETTING_UNSAVED
// is the one that matters: the change is LIVE but did not reach the
// disk, so it disappears at the next boot -- printing that as plain
// success is the exact lie enum setting_result was introduced to stop.
static int report(const char *name, const char *value, uint32_t result) {
    char line[160];
    switch (result) {
    case SETTING_SAVED:
        snprintf(line, sizeof line, "%s = %s", name, value);
        putline(line);
        return 0;
    case SETTING_UNSAVED:
        snprintf(line, sizeof line, "%s = %s  (applied, but NOT saved -- "
                                     "it will revert at the next boot)", name, value);
        putline(line);
        return 1;
    default:
        snprintf(line, sizeof line, "config: '%s' does not accept '%s'", name, value);
        putline(line);
        return 1;
    }
}

// --- reading whole files ---------------------------------------------
//
// Straight bytes, no interpretation. `show` prints them and `find`
// scans them for a substring; neither needs to know the file format,
// which is what keeps a second parser out of this program.
#define FILE_MAX 8192

static int read_file(const char *path, char *buf, int cap) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    int64_t n = sys_read(fd, buf, (size_t)(cap - 1));
    sys_close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

// --- subcommands ------------------------------------------------------

static int cmd_list(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_COUNT)) { putline("config: registry unavailable"); return 1; }
    int n = m.count;

    putline("SETTING           VALUE                 FILE");
    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;

        // A value that differs from the file is flagged rather than
        // hidden: it means someone edited /etc and nothing reloaded,
        // which is a normal state here and the commonest reason a
        // setting "isn't working".
        int edited = m.stored[0] && strcmp(m.value, m.stored) != 0;
        char line[200];
        snprintf(line, sizeof line, "%-17s %-21s %s%s",
                 m.name, m.value, m.file, edited ? "  (file differs -- see `config diff`)" : "");
        putline(line);
    }
    return 0;
}

static int cmd_get(const char *name) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, name, sizeof m.name);
    if (sys_setting(&m) != 0) {
        char line[128];
        snprintf(line, sizeof line, "config: no setting named '%s'", name);
        putline(line);
        return 1;
    }
    putline(m.value);
    return 0;
}

static int cmd_set(const char *name, const char *value) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    strlcpy(m.name, name, sizeof m.name);
    strlcpy(m.value, value, sizeof m.value);
    if (sys_setting(&m) != 0) { putline("config: registry unavailable"); return 1; }

    // A rejected value is worth more than "no": the registry knows the
    // legal ones, so say what they are rather than making the user go
    // and look.
    if (m.result == SETTING_INVALID) {
        int rc = report(name, value, m.result);
        struct setting_msg q;
        if (!op(&q, SETTING_OP_COUNT)) return rc;
        int n = q.count;
        for (int i = 0; i < n; i++) {
            memset(&q, 0, sizeof q);
            q.op = SETTING_OP_INFO;
            q.index = i;
            if (sys_setting(&q) != 0) continue;
            if (strcmp(q.name, name) != 0) continue;
            if (q.type != SETTING_ABI_TYPE_ENUM || q.count <= 0) break;

            put("  try one of:");
            for (int c = 0; c < q.count; c++) {
                struct setting_msg ch;
                memset(&ch, 0, sizeof ch);
                ch.op = SETTING_OP_CHOICE;
                ch.index = i;
                ch.choice = c;
                if (sys_setting(&ch) != 0) break;
                put(" ");
                put(ch.value);
            }
            put("\n");
            break;
        }
        return rc;
    }
    return report(name, value, m.result);
}

static int cmd_unset(const char *name) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_UNSET;
    strlcpy(m.name, name, sizeof m.name);
    if (sys_setting(&m) != 0) { putline("config: registry unavailable"); return 1; }

    char line[160];
    if (m.result != SETTING_SAVED) {
        snprintf(line, sizeof line,
                 "config: '%s' is not a setting, or was not stored anyway", name);
        putline(line);
        return 1;
    }
    // Honest about the half it cannot do: removing the key means the
    // BUILT-IN default applies at the next boot, but nothing here can
    // ask a live subsystem to go back to one.
    snprintf(line, sizeof line,
             "%s removed from its file -- the built-in default applies at the next boot "
             "(the current value stays in effect until then)", name);
    putline(line);
    return 0;
}

static int cmd_where(const char *name) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_COUNT)) return 1;
    int n = m.count;
    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;
        if (strcmp(m.name, name) != 0) continue;
        putline(m.file); // just the path, so it is usable in a command
        return 0;
    }
    char line[128];
    snprintf(line, sizeof line, "config: no setting named '%s'", name);
    putline(line);
    return 1;
}

static int cmd_diff(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_COUNT)) return 1;
    int n = m.count, differ = 0;

    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;
        if (!m.stored[0] || strcmp(m.value, m.stored) == 0) continue;

        char line[200];
        snprintf(line, sizeof line, "%-17s live=%-16s file=%s", m.name, m.value, m.stored);
        putline(line);
        differ++;
    }
    if (!differ) {
        putline("Every setting matches its file.");
        return 0;
    }
    putline("");
    putline("`config reload` applies the file values above.");
    return 0;
}

static int cmd_reload(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_RELOAD)) { putline("config: reload failed"); return 1; }

    // The rejected count is the load-bearing part: a typo in a file
    // someone just edited must not look like the setting simply not
    // working. `count` is how many values the owning subsystem refused.
    char line[128];
    if (m.count > 0) {
        snprintf(line, sizeof line,
                 "Reloaded -- %d value(s) REFUSED (see `config diff`); "
                 "the previous working value is still in effect for those.", m.count);
        putline(line);
        return 1;
    }
    putline("Reloaded.");
    return 0;
}

static int cmd_files(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_FILE_COUNT)) return 1;
    int n = m.count;

    putline("NAME        PATH                   SOURCE    DESCRIPTION");
    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_FILE_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;

        char line[220];
        snprintf(line, sizeof line, "%-11s %-22s %-9s %s%s",
                 m.name, m.file,
                 m.type ? "built-in" : "config.d",
                 m.label,
                 // A descriptor may name a file nothing has written yet.
                 // Reported rather than filtered: hiding it leaves
                 // "where will my settings go?" unanswerable until after
                 // the first save.
                 m.count ? "" : "  (not created yet)");
        putline(line);
    }
    return 0;
}

static int cmd_show(const char *which) {
    struct setting_msg m;
    char path[SETTING_ABI_FILE_MAX];

    // Accept either the umbrella name (`config show system`) or a bare
    // path, so this composes with `config where`.
    path[0] = '\0';
    if (which[0] == '/') {
        strlcpy(path, which, sizeof path);
    } else {
        if (!op(&m, SETTING_OP_FILE_COUNT)) return 1;
        int n = m.count;
        for (int i = 0; i < n; i++) {
            memset(&m, 0, sizeof m);
            m.op = SETTING_OP_FILE_INFO;
            m.index = i;
            if (sys_setting(&m) != 0) continue;
            if (strcmp(m.name, which) != 0) continue;
            strlcpy(path, m.file, sizeof path);
            break;
        }
    }
    if (!path[0]) {
        char line[160];
        snprintf(line, sizeof line,
                 "config: no config file called '%s' -- `config files` lists them", which);
        putline(line);
        return 1;
    }

    static char buf[FILE_MAX];
    int n = read_file(path, buf, sizeof buf);
    if (n < 0) {
        char line[160];
        snprintf(line, sizeof line, "config: %s does not exist yet", path);
        putline(line);
        return 1;
    }
    char head[160];
    snprintf(head, sizeof head, "--- %s ---", path);
    putline(head);
    put(buf);
    if (n > 0 && buf[n - 1] != '\n') put("\n");
    return 0;
}

// Case-insensitive substring, so `config find HELSINKI` finds a
// lowercase city -- the same tolerance tz_find_by_name() has, and for
// the same reason: it is what someone actually types.
static int line_contains(const char *hay, int hay_len, const char *needle) {
    int nl = (int)strlen(needle);
    if (nl == 0 || nl > hay_len) return 0;
    for (int i = 0; i + nl <= hay_len; i++) {
        int j = 0;
        while (j < nl) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static int cmd_find(const char *text) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_FILE_COUNT)) return 1;
    int n = m.count, hits = 0;

    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_FILE_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;
        if (!m.count) continue; // not created yet

        static char buf[FILE_MAX];
        char path[SETTING_ABI_FILE_MAX];
        strlcpy(path, m.file, sizeof path);
        int len = read_file(path, buf, sizeof buf);
        if (len <= 0) continue; // a directory, or unreadable -- see below

        // Whole LINES, grep-style. Matching keys and values with one
        // search is not a shortcut: it is what makes "which config has
        // this value?" and "which config has this setting name?" the
        // same question, which is how they were asked for.
        int start = 0;
        for (int p = 0; p <= len; p++) {
            if (p != len && buf[p] != '\n') continue;
            int line_len = p - start;
            if (line_len > 0 && line_contains(buf + start, line_len, text)) {
                char out[240];
                int copy = line_len;
                if (copy > (int)sizeof out - 80) copy = (int)sizeof out - 80;
                char frag[160];
                memcpy(frag, buf + start, (size_t)copy);
                frag[copy] = '\0';
                snprintf(out, sizeof out, "%s:%s", path, frag);
                putline(out);
                hits++;
            }
            start = p + 1;
        }
    }
    if (!hits) {
        char line[160];
        snprintf(line, sizeof line, "Nothing matching '%s' in any registered config file.", text);
        putline(line);
        return 1;
    }
    return 0;
}

// --- registering a config file ---------------------------------------
//
// Writing a descriptor file IS the registration -- there is no syscall
// for it, on purpose. A config file is declared by a file, the same way
// an app is declared by a `.desktop` entry, which is what lets a ring-3
// program (the window manager, after Milestone 41) declare its own
// config with no kernel change.

static int descriptor_path(const char *name, char *out, int cap) {
    if (!name[0]) return 0;
    for (const char *p = name; *p; p++) {
        // A name is an identifier, not a path: one that could contain a
        // slash would let `config register` write outside /etc/config.d.
        if (*p == '/' || *p == '.' || *p == ' ') return 0;
    }
    return snprintf(out, (size_t)cap, "/etc/config.d/%s.conf", name) > 0;
}

static int cmd_register(const char *name, const char *path, const char *desc) {
    char dpath[96];
    if (!descriptor_path(name, dpath, sizeof dpath)) {
        putline("config: a name must be a plain word -- no '/', '.' or spaces");
        return 1;
    }
    if (path[0] != '/') {
        putline("config: the path must be absolute");
        return 1;
    }

    char body[320];
    snprintf(body, sizeof body,
             "# Registered by `config register`. Delete this file (or run\n"
             "# `config unregister %s`) to remove it from `config files`.\n"
             "Name=%s\nPath=%s\nDescription=%s\n",
             name, name, path, desc ? desc : "");

    int fd = sys_open(dpath, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) {
        putline("config: could not write the descriptor -- does /etc/config.d exist?");
        return 1;
    }
    int64_t w = sys_write(fd, body, strlen(body));
    sys_close(fd);
    if (w < 0) { putline("config: writing the descriptor failed"); return 1; }

    // The kernel rescans on reload, so do it here rather than leaving
    // the user to discover that `config files` has not changed yet.
    struct setting_msg m;
    op(&m, SETTING_OP_RELOAD);

    char line[200];
    snprintf(line, sizeof line, "Registered '%s' -> %s (descriptor: %s)", name, path, dpath);
    putline(line);
    return 0;
}

static int cmd_unregister(const char *name) {
    char dpath[96];
    if (!descriptor_path(name, dpath, sizeof dpath)) {
        putline("config: a name must be a plain word");
        return 1;
    }
    // sys_unlink() returns 1 on SUCCESS, not 0 -- this kernel's syscall
    // return conventions are per-call and documented per-call in
    // abi/syscall_abi.h; assuming the C `0 == ok` idiom here inverted
    // the check and made a successful unregister report failure while
    // having actually deleted the descriptor.
    if (!sys_unlink(dpath)) {
        char line[200];
        snprintf(line, sizeof line,
                 "config: '%s' has no descriptor in /etc/config.d "
                 "(a BUILT-IN cannot be unregistered -- see `config files`)", name);
        putline(line);
        return 1;
    }
    struct setting_msg m;
    op(&m, SETTING_OP_RELOAD);
    char line[160];
    snprintf(line, sizeof line, "Unregistered '%s'.", name);
    putline(line);
    return 0;
}

// --- usage ------------------------------------------------------------

static int usage(void) {
    putline("config -- read, change and find this machine's configuration");
    putline("");
    putline("Settings (typed, validated, applied):");
    putline("  config list                 every setting, its value, and its file");
    putline("  config get <name>           one value");
    putline("  config set <name> <value>   validate, apply and persist");
    putline("  config set <name>=<value>   the same");
    putline("  config unset <name>         forget it, so the built-in default returns");
    putline("  config where <name>         just the file holding it");
    putline("  config diff                 settings whose file differs from what is live");
    putline("  config reload               re-read the files after editing them by hand");
    putline("");
    putline("Config files:");
    putline("  config files                every known config file, and where it came from");
    putline("  config show <name|path>     print one, verbatim");
    putline("  config find <text>          search names AND values across all of them");
    putline("  config register <name> <path> [description]");
    putline("  config unregister <name>");
    putline("");
    putline("Settings are ordinary text under /etc -- `edit` them freely, then");
    putline("`config reload`. `config diff` shows what an edit has not applied yet.");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) return usage();
    if (!strcmp(cmd, "list"))   return cmd_list();
    if (!strcmp(cmd, "diff"))   return cmd_diff();
    if (!strcmp(cmd, "reload")) return cmd_reload();
    if (!strcmp(cmd, "files"))  return cmd_files();

    if (!strcmp(cmd, "get") && argc >= 3)   return cmd_get(argv[2]);
    if (!strcmp(cmd, "where") && argc >= 3) return cmd_where(argv[2]);
    if (!strcmp(cmd, "unset") && argc >= 3) return cmd_unset(argv[2]);
    if (!strcmp(cmd, "show") && argc >= 3)  return cmd_show(argv[2]);
    if (!strcmp(cmd, "find") && argc >= 3)  return cmd_find(argv[2]);
    if (!strcmp(cmd, "unregister") && argc >= 3) return cmd_unregister(argv[2]);

    if (!strcmp(cmd, "register") && argc >= 4) {
        // The description is the whole REST of the line, rejoined --
        // it is prose, and the shell split it on spaces. Taking only
        // argv[4] silently registered "Settings" for a description
        // someone typed as "Settings for my app".
        char desc[SETTING_ABI_FILE_MAX * 2];
        size_t used = 0;
        desc[0] = '\0';
        for (int i = 4; i < argc && used + 1 < sizeof desc; i++) {
            used += snprintf(desc + used, sizeof desc - used, "%s%s",
                             used ? " " : "", argv[i]);
        }
        return cmd_register(argv[2], argv[3], desc);
    }

    if (!strcmp(cmd, "set") && argc >= 3) {
        // Both spellings, because both are what people type. `name=value`
        // is split here rather than in the shell so the whole thing can
        // arrive as one argument.
        char name[SETTING_ABI_NAME_MAX];
        const char *eq = 0;
        for (const char *p = argv[2]; *p; p++) { if (*p == '=') { eq = p; break; } }
        if (eq) {
            int n = (int)(eq - argv[2]);
            if (n >= (int)sizeof name) { putline("config: name too long"); return 1; }
            memcpy(name, argv[2], (size_t)n);
            name[n] = '\0';
            return cmd_set(name, eq + 1);
        }
        if (argc >= 4) return cmd_set(argv[2], argv[3]);
        putline("config: set needs a value -- `config set <name> <value>`");
        return 1;
    }

    char line[128];
    snprintf(line, sizeof line, "config: unknown subcommand '%s'", cmd);
    putline(line);
    putline("");
    return usage();
}
