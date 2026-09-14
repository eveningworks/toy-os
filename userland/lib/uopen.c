// uopen -- see lib/uopen.h for the two layers and why there is no
// daemon. The declaration scan moved here from the File Manager, which
// was the design doc's "the day a second caller wants one".
#include <stdlib.h>
#include "lib/uopen.h"
#include "lib/uconf.h"
#include "etc_config.h"
#include "rt/sys.h"
#include "kpath.h"
#include <string.h>
#include <strings.h> // strncasecmp -- an extension is not case-sensitive
#include <stdio.h>
#include <ctype.h>

#define UOPEN_PATH_MAX 64 // FS_PATH_MAX
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

int uopen_spawn(const char *path) {
    char exec[UOPEN_PATH_MAX];
    if (uopen_resolve(path, exec, sizeof exec))
        return sys_spawn(exec, path, -1);
    // A directory opens where directories live -- Explorer's rule.
    struct sys_dirent probe;
    if (sys_listdir(path, &probe, 1) >= 0)
        return sys_spawn("/bin/wm/apps/files", path, -1);
    return -1;
}
