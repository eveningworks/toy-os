// See lib/uappentry.h.
#include "lib/uappentry.h"
#include "lib/uconf.h"
#include "lib/uopen.h"   // UOPEN_ENTRY_SECTION
#include "rt/sys.h"
#include "kpath.h"
#include <stdlib.h>
#include <string.h>

static void field(struct etc_config_buf *cfg, const char *key, char *out, int cap) {
    if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, key, out, cap)) out[0] = '\0';
}

int uappentry_read(const char *path, struct uappentry *out) {
    memset(out, 0, sizeof *out);
    // The parse buffer is a few KiB: too big for a ring-3 frame.
    struct etc_config_buf *cfg = malloc(sizeof *cfg);
    if (!cfg) return 0;
    int ok = uconf_load(path, cfg);
    if (ok) {
        field(cfg, "Exec", out->exec, sizeof out->exec);
        field(cfg, "Name", out->name, sizeof out->name);
        field(cfg, "Icon", out->icon, sizeof out->icon);
        field(cfg, "Category", out->category, sizeof out->category);
    }
    free(cfg);
    char *sp = strchr(out->exec, ' ');
    if (sp) *sp = '\0';
    if (!ok || !out->exec[0]) return 0;

    const char *base = k_path_basename(path);
    strlcpy(out->stem, base, sizeof out->stem);
    char *dot = strstr(out->stem, ".desktop");
    if (dot && dot[8] == '\0') *dot = '\0';
    if (!out->name[0]) strlcpy(out->name, out->stem, sizeof out->name);
    return 1;
}

static int is_entry(const char *name) {
    size_t n = strlen(name);
    return n > 8 && strcmp(name + n - 8, ".desktop") == 0;
}

int uappentry_each(int (*fn)(const struct uappentry *e, void *ctx), void *ctx) {
    static struct sys_dirent ents[SYS_LISTDIR_MAX];
    int n = sys_listdir(UAPPENTRY_DIR, ents, SYS_LISTDIR_MAX);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir || !is_entry(ents[i].name)) continue;
        char path[160];
        if (!k_path_join(UAPPENTRY_DIR, ents[i].name, path, sizeof path)) continue;
        struct uappentry e;
        if (!uappentry_read(path, &e)) continue;
        seen++;
        if (fn(&e, ctx)) break;
    }
    return seen;
}

struct find { const char *exec; struct uappentry *out; int hit; };

static int match(const struct uappentry *e, void *ctx) {
    struct find *f = ctx;
    if (strcmp(e->exec, f->exec) != 0) return 0;
    *f->out = *e;
    f->hit = 1;
    return 1;
}

int uappentry_find_exec(const char *exec, struct uappentry *out) {
    if (!exec || !exec[0]) return 0;
    struct find f = { exec, out, 0 };
    uappentry_each(match, &f);
    return f.hit;
}
