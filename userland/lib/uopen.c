// uopen -- see lib/uopen.h for the two layers and why there is no
// daemon. The declaration scan moved here from the File Manager, which
// was the design doc's "the day a second caller wants one".
#include <stdlib.h>
#include "lib/uopen.h"
#include "lib/ulaunch.h"
#include "lib/urecent.h"
#include "lib/uconf.h"
#include "etc_config.h"
#include "rt/sys.h"
#include "kpath.h"
#include <string.h>
#include <strings.h> // strncasecmp -- an extension is not case-sensitive
#include <stdio.h>
#include <ctype.h>

#define UOPEN_PATH_MAX 256 // NOT FS_PATH_MAX (4096): this file's own bound
#define DESKTOP_ENTRY_DIR "/usr/wm/applications"

// A space/comma-separated list, matched whole -- ".md" must not match
// ".mdx", which a substring search would. See uopen.h.
int uopen_ext_matches(const char *list, const char *ext) {
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != ',') p++;
        int n = (int)(p - start);
        if (n > 0 && (int)strlen(ext) == n && strncasecmp(start, ext, (size_t)n) == 0)
            return 1;
    }
    return 0;
}

// One entry's Exec, by its name (the filename stem). 1 = found.
static int entry_exec(const char *name, char *out, int cap) {
    char entry[UOPEN_PATH_MAX];
    if (snprintf(entry, sizeof entry, DESKTOP_ENTRY_DIR "/%s.desktop", name)
        >= (int)sizeof entry)
        return 0;
    // Heap: the whole document does not fit a ring-3 frame -- see
    // uconf.c's note.
    struct etc_config_buf *cfg = malloc(sizeof *cfg);
    if (!cfg) return 0;
    int rc = uconf_load(entry, cfg)
             && etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Exec",
                                             out, (uint32_t)cap);
    free(cfg);
    return rc ? 1 : 0;
}

// The declaration scan: the first entry whose Handles= claims `ext`.
static int declared_exec(const char *ext, char *out, int cap) {
    static struct sys_dirent entries[SYS_LISTDIR_MAX];
    int n = sys_listdir(DESKTOP_ENTRY_DIR, entries, SYS_LISTDIR_MAX);
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        char entry[UOPEN_PATH_MAX];
        if (!k_path_join(DESKTOP_ENTRY_DIR, entries[i].name, entry, sizeof entry))
            continue;
        // Loaded ONCE and asked twice: etc_config_get() re-reads the
        // whole file per key (CLAUDE.md's 54-reads lesson).
        struct etc_config_buf *cfg = malloc(sizeof *cfg);
        if (!cfg) continue;
        // A Handles= list is a handful of extensions, not a document --
        // it is sized independently of the buffer so raising that one
        // does not put a KiB of list on this frame.
        char list[128];
        int hit = uconf_load(entry, cfg)
                  && etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION,
                                                  "Handles", list, sizeof list)
                  && uopen_ext_matches(list, ext)
                  && etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION,
                                                  "Exec", out, (uint32_t)cap);
        free(cfg);
        if (hit) return 1;
    }
    return 0;
}

int uopen_resolve(const char *path, char *exec, int cap) {
    const char *dot = strrchr(k_path_basename(path), '.');
    if (!dot || !dot[1]) return 0;

    // The override file's keys are stored lowercased; matching them
    // case-insensitively at both ends is what makes ".JPG" one type.
    char ext[16];
    int n = 0;
    for (; dot[n] && n < (int)sizeof ext - 1; n++)
        ext[n] = (char)tolower((unsigned char)dot[n]);
    ext[n] = '\0';

    // `[Default Applications]` first, then the TOP LEVEL: a
    // mimeapps.list copied off a Linux box carries the header, ours does
    // not, and `open -s` keeps writing the flat form (uopen.h). Asking
    // only the section would stop resolving every override already on
    // disk. Loaded once rather than asked twice, since each ask is a
    // whole-file read.
    char val[UOPEN_PATH_MAX];
    val[0] = '\0';
    struct etc_config_buf *conf = malloc(sizeof *conf);
    if (conf) {
        if (uconf_load(UOPEN_CONF, conf))
            etc_config_buf_get_in_or_top(conf, UOPEN_CONF_SECTION, ext,
                                         val, sizeof val);
        free(conf);
    }
    if (val[0] && strcmp(val, "-") != 0) {
        if (val[0] == '/') { // the literal-path escape hatch
            strlcpy(exec, val, (size_t)cap);
            return 1;
        }
        if (entry_exec(val, exec, cap)) return 1;
        // A dangling override falls through to the declarations.
    }
    return declared_exec(ext, exec, cap);
}

// The path as ONE argument: the string form of a spawn splits on
// whitespace, which cut "my notes.txt" to "my".
static int spawn_with(const char *exec, const char *path) {
    char *const argv[] = { (char *)exec, (char *)path, 0 };
    return sys_spawn_argv(exec, argv);
}

// A program or a script is RUN, not opened (lib/ulaunch.h): an app at
// once, the rest by the policy -- and asking is /bin/wm/system/runask's
// card, since this caller may have no window to put it on.
static int launch(const char *path) {
    struct ulaunch_info li;
    if (!ulaunch_classify(path, &li) || li.kind == ULAUNCH_NONE) return -2;
    int act = li.runnable && li.interp_found ? ulaunch_policy(li.kind) : ULAUNCH_ASK;
    char *argv[5];
    if (act == ULAUNCH_ASK) {
        argv[0] = ULAUNCH_ASK_EXEC;
        argv[1] = (char *)path;
        argv[2] = 0;
    } else if (!ulaunch_argv(path, li.kind, act, argv)) {
        return -1;
    }
    return sys_spawn_argv(argv[0], argv);
}

int uopen_spawn(const char *path) {
    int pid = launch(path);
    if (pid != -2) return pid;
    char exec[UOPEN_PATH_MAX];
    if (uopen_resolve(path, exec, sizeof exec)) {
        int pid = spawn_with(exec, path);
        if (pid >= 0) {   // a file opened for a person: Recent's (lib/urecent.h)
            char app[URECENT_APP];
            urecent_app_of_exec(exec, app, sizeof app);
            urecent_add(path, app);
        }
        return pid;
    }
    // A directory opens where directories live -- Explorer's rule.
    struct sys_dirent probe;
    if (sys_listdir(path, &probe, 1) >= 0)
        return spawn_with("/bin/wm/apps/files", path);
    return -1;
}

// The extension, lowercased, as the override file keys it. 0 for none.
static int ext_of(const char *path, char *ext, int cap) {
    const char *dot = strrchr(k_path_basename(path), '.');
    if (!dot || !dot[1]) return 0;
    int n = 0;
    for (; dot[n] && n < cap - 1; n++) ext[n] = (char)tolower((unsigned char)dot[n]);
    ext[n] = '\0';
    return 1;
}

int uopen_apps_for(const char *path, struct uopen_app *out, int max, int *current) {
    static struct sys_dirent entries[SYS_LISTDIR_MAX];
    char ext[16], now[UOPEN_PATH_MAX] = "";
    int count = 0;
    *current = -1;
    if (!ext_of(path, ext, sizeof ext)) return 0;
    if (!uopen_resolve(path, now, sizeof now)) now[0] = '\0';
    int n = sys_listdir(DESKTOP_ENTRY_DIR, entries, SYS_LISTDIR_MAX);
    struct etc_config_buf *cfg = malloc(sizeof *cfg);
    if (!cfg) return 0;
    // Two passes, Windows' "Open with" order: the apps that claim this
    // extension, then every other app that opens files at all.
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n && count < max; i++) {
        const char *nm = entries[i].name;
        size_t len = strlen(nm);
        if (entries[i].is_dir || len < 9 || strcmp(nm + len - 8, ".desktop")) continue;
        char entry[UOPEN_PATH_MAX], list[128];
        struct uopen_app *a = &out[count];
        if (!k_path_join(DESKTOP_ENTRY_DIR, nm, entry, sizeof entry) || !uconf_load(entry, cfg)) continue;
        if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Handles", list, sizeof list) ||
            !list[0] || uopen_ext_matches(list, ext) != (pass == 0) ||
            !etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Exec", a->exec, sizeof a->exec))
            continue;
        if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Name", a->name, sizeof a->name))
            strlcpy(a->name, k_path_basename(a->exec), sizeof a->name);
        snprintf(a->entry, sizeof a->entry, "%.*s", (int)(len - 8), nm);
        if (now[0] && !strcmp(now, a->exec)) *current = count;
        count++;
    }
    free(cfg);
    // An override naming a program no entry claims still opens the file:
    // list it, or the choice would vanish from the list showing it.
    if (now[0] && *current < 0 && count < max) {
        struct uopen_app *a = &out[count];
        strlcpy(a->exec, now, sizeof a->exec);
        strlcpy(a->name, k_path_basename(now), sizeof a->name);
        strlcpy(a->entry, now, sizeof a->entry);
        *current = count++;
    }
    return count;
}

int uopen_set_default(const char *path, const char *entry) {
    char ext[16];
    if (!ext_of(path, ext, sizeof ext)) return -1;
    return uconf_set(UOPEN_CONF, ext, entry) ? 0 : -1;
}

