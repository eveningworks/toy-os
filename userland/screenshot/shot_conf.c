// Screenshot's options file, /etc/screenshot.conf: `name=value`, booleans
// as on/off -- one lib/uprefs.h table, Notepad's arrangement (np_conf.c),
// the folder and the name template included, each with its own check.
#include "screenshot.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rt/sys.h"
#include "keycombo.h"
#include "lib/uconf.h"
#include "lib/uprefs.h"
#include "lib/usetting.h"

const char *const SHOT_MODE_NAME[SHOT_MODES] = { "region", "screen", "window" };
static const char *const START_WORDS[SHOT_MODES + 1] = { "region", "screen", "window", "last" };

// A template is checked against today's values, which is all `valid` can
// say about one that names a token no capture would fill.
static int name_ok(const char *s) {
    struct shot_vars sv;
    shot_vars_now(&sv, "", 0);
    return unametpl_valid(s, sv.v, SHOT_CHIP_COUNT);
}

#define C struct shot_conf
static const struct upref KEYS[] = {
    UPREF_WORD_KEY("mode",        C, mode,        SHOT_MODE_NAME, SHOT_MODES, SHOT_MODE_REGION),
    UPREF_BOOL_KEY("pointer",     C, pointer,     0),
    UPREF_BOOL_KEY("copy",        C, copy,        0),
    UPREF_INT_KEY("delay",        C, delay,       0, 60, 0),
    UPREF_WORD_KEY("start",       C, start,       START_WORDS, SHOT_MODES + 1, SHOT_START_LAST),
    UPREF_BOOL_KEY("last_region", C, last_region, 1),
    UPREF_BOOL_KEY("size_label",  C, size_label,  1),
    UPREF_BOOL_KEY("magnifier",   C, magnifier,   0),
    UPREF_BOOL_KEY("shadow",      C, shadow,      0),
    UPREF_INT_KEY("delay1",       C, delays[0],   1, 60, 3),
    UPREF_INT_KEY("delay2",       C, delays[1],   1, 60, 5),
    UPREF_INT_KEY("delay3",       C, delays[2],   1, 60, 10),
    UPREF_INT_KEY("region_x",     C, rx,          0, 16384, 0),
    UPREF_INT_KEY("region_y",     C, ry,          0, 16384, 0),
    UPREF_INT_KEY("region_w",     C, rw,          0, 16384, 0),
    UPREF_INT_KEY("region_h",     C, rh,          0, 16384, 0),
    UPREF_INT_KEY("region_sw",    C, rsw,         0, 16384, 0),
    UPREF_INT_KEY("region_sh",    C, rsh,         0, 16384, 0),
    UPREF_BOOL_KEY("card",        C, card,        1),
    UPREF_BOOL_KEY("open",        C, open,        0),
    UPREF_BOOL_KEY("flash",       C, flash,       0),
    UPREF_BOOL_KEY("png",         C, png,         0),
    UPREF_BOOL_KEY("ask",         C, ask,         0),
    UPREF_TEXT_KEY("folder",      C, folder,      SHOT_DEFAULT_DIR, shot_folder_ok),
    UPREF_TEXT_KEY("name",        C, name,        SHOT_DEFAULT_NAME, name_ok),
};
#undef C
static const struct uprefs PREFS = { SHOT_CONF, KEYS, (int)(sizeof KEYS / sizeof KEYS[0]) };

void shot_conf_defaults(struct shot_conf *c) {
    memset(c, 0, sizeof *c);
    uprefs_defaults(&PREFS, c);
}

void shot_conf_load(struct shot_conf *c) {
    memset(c, 0, sizeof *c);
    uprefs_load(&PREFS, c);
}

void shot_conf_save(const struct shot_conf *c) {
    struct shot_conf was;
    shot_conf_load(&was);
    uprefs_save(&PREFS, c, &was);
}

// --- naming -----------------------------------------------------------

const struct uui_nametpl_chip SHOT_CHIPS[SHOT_CHIP_COUNT] = {
    { "Date", "date" }, { "Time", "time" }, { "App", "app" }, { "Mode", "mode" }, { "Counter", "n" },
};

void shot_vars_now(struct shot_vars *s, const char *app, int mode) {
    memset(s, 0, sizeof *s);
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) {
        strftime(s->date, sizeof s->date, "%Y%m%d", &tm);
        strftime(s->time, sizeof s->time, "%H%M%S", &tm);
    }
    unametpl_clean(app, s->app, sizeof s->app);
    snprintf(s->n, sizeof s->n, "1");
    const char *names[SHOT_CHIP_COUNT] = { "date", "time", "app", "mode", "n" };
    const char *vals[SHOT_CHIP_COUNT] = { s->date, s->time, s->app,
                                          mode >= 0 && mode < SHOT_MODES ? SHOT_MODE_NAME[mode] : "",
                                          s->n };
    for (int i = 0; i < SHOT_CHIP_COUNT; i++) { s->v[i].name = names[i]; s->v[i].value = vals[i]; }
}

static int taken(const char *path) {
    struct sys_stat st;
    return sys_stat(path, &st) == 0;
}

int shot_next_path(const struct shot_conf *c, const char *app, int mode, char *out, int cap) {
    struct shot_vars sv;
    shot_vars_now(&sv, app, mode);
    const char *ext = c->png ? ".png" : ".qoi";
    char name[SHOT_PATH_MAX];
    int counted = strstr(c->name, "<n>") != 0;
    // <n> COUNTS UP to the first free name; without it, a second capture
    // in the same second gets -2, -3 (the first file must not be written
    // over: its card points at it).
    for (int n = 1; n < 1000; n++) {
        snprintf(sv.n, sizeof sv.n, "%d", n);
        if (!unametpl_expand(c->name, sv.v, SHOT_CHIP_COUNT, name, sizeof name)) return 0;
        int w;
        if (counted || n == 1) w = snprintf(out, (size_t)cap, "%s/%s%s", c->folder, name, ext);
        else w = snprintf(out, (size_t)cap, "%s/%s-%d%s", c->folder, name, n, ext);
        if (w <= 0 || w >= cap) return 0;
        if (!taken(out)) return 1;
    }
    return 0;
}

int shot_folder_ok(const char *dir) {
    if (!dir || dir[0] != '/') return 0;
    size_t n = strlen(dir);
    if (n >= SHOT_DIR_MAX) return 0;
    for (const char *p = dir; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                 *p == '/' || *p == '.' || *p == '_' || *p == '-';
        if (!ok || (p[0] == '.' && p[1] == '.')) return 0;
    }
    return 1;
}

// --- the Print Screen keys ---------------------------------------------

const char *const SHOT_KEY_COMBO[SHOT_KEYS] = { "Print Screen", "Shift+Print Screen", "Alt+Print Screen" };
const char *const SHOT_KEY_LABEL[SHOT_KEY_CHOICES] = {
    "Opens the capture bar", "Saves the whole screen", "Saves the active window",
    "Selects a region", "Does nothing",
};
static const char *const ACTION[SHOT_KEY_NONE] = {
    "shortcuts.screenshot", "shortcuts.screenshot_screen", "shortcuts.screenshot_window",
    "shortcuts.screenshot_region",
};

static int same(const struct keycombo *a, const struct keycombo *b) {
    return a->key == b->key && a->mods == b->mods;
}

// Calls `each` with every combination in a binding list, trimmed.
static void walk(const char *list, void (*each)(const char *one, void *ctx), void *ctx) {
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - s);
        while (len > 0 && s[len - 1] == ' ') len--;
        char one[KEYCOMBO_TEXT_MAX];
        if (len <= 0 || len >= (int)sizeof one) continue;
        memcpy(one, s, (size_t)len);
        one[len] = '\0';
        each(one, ctx);
    }
}

struct find_ctx { int action; int *which; };
static void note_key(const char *one, void *vctx) {
    struct find_ctx *f = vctx;
    struct keycombo c, k;
    if (!keycombo_parse(one, &c)) return;
    for (int i = 0; i < SHOT_KEYS; i++)
        if (keycombo_parse(SHOT_KEY_COMBO[i], &k) && same(&c, &k)) f->which[i] = f->action;
}

void shot_keys_get(int which[SHOT_KEYS]) {
    for (int i = 0; i < SHOT_KEYS; i++) which[i] = SHOT_KEY_NONE;
    for (int a = 0; a < SHOT_KEY_NONE; a++) {
        char v[128];
        if (usetting_get(ACTION[a], v, sizeof v) != 1) continue;
        struct find_ctx f = { a, which };
        walk(v, note_key, &f);
    }
}

struct keep_ctx { char *out; size_t cap; };
static void keep_other(const char *one, void *vctx) {
    struct keep_ctx *k = vctx;
    struct keycombo c, s;
    if (!keycombo_parse(one, &c)) return;
    for (int i = 0; i < SHOT_KEYS; i++)
        if (keycombo_parse(SHOT_KEY_COMBO[i], &s) && same(&c, &s)) return;
    size_t n = strlen(k->out);
    snprintf(k->out + n, k->cap - n, "%s%s", n ? ", " : "", one);
}

int shot_keys_set(const int which[SHOT_KEYS]) {
    int ok = 1;
    for (int a = 0; a < SHOT_KEY_NONE; a++) {
        char was[128], next[128] = "";
        if (usetting_get(ACTION[a], was, sizeof was) != 1) was[0] = '\0';
        struct keep_ctx k = { next, sizeof next };
        walk(was, keep_other, &k);
        for (int i = 0; i < SHOT_KEYS; i++) {
            if (which[i] != a) continue;
            size_t n = strlen(next);
            snprintf(next + n, sizeof next - n, "%s%s", n ? ", " : "", SHOT_KEY_COMBO[i]);
        }
        if (strcmp(was, next) && usetting_set(ACTION[a], next) < 0) ok = 0;
    }
    return ok;
}
