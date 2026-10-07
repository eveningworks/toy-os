// Screenshot's options file, /etc/screenshot.conf: `name=value`, booleans
// as on/off, through lib/uconf.h -- Notepad's arrangement (np_conf.c).
// ONE TABLE names every number-valued key, so load, save and the
// defaults cannot disagree; the two strings are handled beside it.
#include "screenshot.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rt/sys.h"
#include "keycombo.h"
#include "lib/uconf.h"
#include "lib/usetting.h"

const char *const SHOT_MODE_NAME[SHOT_MODES] = { "region", "screen", "window" };
static const char *const START_WORDS[SHOT_MODES + 1] = { "region", "screen", "window", "last" };

enum { K_BOOL, K_INT, K_WORD };

static const struct key {
    const char *name;
    size_t off;
    int kind;
    int lo, hi;                  // K_INT's range; K_WORD's count in `hi`
    const char *const *words;
} KEYS[] = {
    { "mode",         offsetof(struct shot_conf, mode),        K_WORD, 0, SHOT_MODES, SHOT_MODE_NAME },
    { "pointer",      offsetof(struct shot_conf, pointer),     K_BOOL, 0, 0, 0 },
    { "copy",         offsetof(struct shot_conf, copy),        K_BOOL, 0, 0, 0 },
    { "delay",        offsetof(struct shot_conf, delay),       K_INT,  0, 60, 0 },
    { "start",        offsetof(struct shot_conf, start),       K_WORD, 0, SHOT_MODES + 1, START_WORDS },
    { "last_region",  offsetof(struct shot_conf, last_region), K_BOOL, 0, 0, 0 },
    { "size_label",   offsetof(struct shot_conf, size_label),  K_BOOL, 0, 0, 0 },
    { "magnifier",    offsetof(struct shot_conf, magnifier),   K_BOOL, 0, 0, 0 },
    { "shadow",       offsetof(struct shot_conf, shadow),      K_BOOL, 0, 0, 0 },
    { "delay1",       offsetof(struct shot_conf, delays[0]),   K_INT,  1, 60, 0 },
    { "delay2",       offsetof(struct shot_conf, delays[1]),   K_INT,  1, 60, 0 },
    { "delay3",       offsetof(struct shot_conf, delays[2]),   K_INT,  1, 60, 0 },
    { "region_x",     offsetof(struct shot_conf, rx),          K_INT,  0, 16384, 0 },
    { "region_y",     offsetof(struct shot_conf, ry),          K_INT,  0, 16384, 0 },
    { "region_w",     offsetof(struct shot_conf, rw),          K_INT,  0, 16384, 0 },
    { "region_h",     offsetof(struct shot_conf, rh),          K_INT,  0, 16384, 0 },
    { "region_sw",    offsetof(struct shot_conf, rsw),         K_INT,  0, 16384, 0 },
    { "region_sh",    offsetof(struct shot_conf, rsh),         K_INT,  0, 16384, 0 },
    { "card",         offsetof(struct shot_conf, card),        K_BOOL, 0, 0, 0 },
    { "open",         offsetof(struct shot_conf, open),        K_BOOL, 0, 0, 0 },
    { "flash",        offsetof(struct shot_conf, flash),       K_BOOL, 0, 0, 0 },
    { "png",          offsetof(struct shot_conf, png),         K_BOOL, 0, 0, 0 },
    { "ask",          offsetof(struct shot_conf, ask),         K_BOOL, 0, 0, 0 },
};
#define NKEYS ((int)(sizeof KEYS / sizeof KEYS[0]))

static int *field(struct shot_conf *c, const struct key *k) { return (int *)((char *)c + k->off); }
static int value(const struct shot_conf *c, const struct key *k) {
    return *(const int *)((const char *)c + k->off);
}

void shot_conf_defaults(struct shot_conf *c) {
    memset(c, 0, sizeof *c);
    c->mode = SHOT_MODE_REGION;
    c->start = SHOT_START_LAST;
    c->last_region = 1;
    c->size_label = 1;
    c->delays[0] = 3; c->delays[1] = 5; c->delays[2] = 10;
    c->card = 1;
    snprintf(c->folder, sizeof c->folder, "%s", SHOT_DEFAULT_DIR);
    snprintf(c->name, sizeof c->name, "%s", SHOT_DEFAULT_NAME);
}

// A value this cannot read leaves the default: a typo in the file is a
// key ignored, never an option silently set to zero.
static void parse(struct shot_conf *c, const struct key *k, const char *v) {
    if (k->kind == K_BOOL) {
        if (!strcmp(v, "on")) *field(c, k) = 1;
        else if (!strcmp(v, "off")) *field(c, k) = 0;
    } else if (k->kind == K_WORD) {
        for (int w = 0; w < k->hi; w++)
            if (!strcmp(v, k->words[w])) *field(c, k) = w;
    } else {
        int n = 0, any = 0;
        for (const char *p = v; *p >= '0' && *p <= '9' && n < 100000; p++) { n = n * 10 + (*p - '0'); any = 1; }
        if (any && n >= k->lo && n <= k->hi) *field(c, k) = n;
    }
}

void shot_conf_load(struct shot_conf *c) {
    shot_conf_defaults(c);
    static struct etc_config_buf buf;   // 4 KiB: never on a ring-3 frame
    if (!uconf_load(SHOT_CONF, &buf)) return;
    for (int i = 0; i < NKEYS; i++) {
        char v[32];
        if (etc_config_buf_get(&buf, KEYS[i].name, v, sizeof v)) parse(c, &KEYS[i], v);
    }
    char s[SHOT_DIR_MAX];
    if (etc_config_buf_get(&buf, "folder", s, sizeof s) && shot_folder_ok(s))
        snprintf(c->folder, sizeof c->folder, "%s", s);
    struct shot_vars sv;
    shot_vars_now(&sv, "", 0);
    if (etc_config_buf_get(&buf, "name", s, sizeof s) && unametpl_valid(s, sv.v, SHOT_CHIP_COUNT))
        snprintf(c->name, sizeof c->name, "%s", s);
}

void shot_conf_save(const struct shot_conf *c) {
    struct shot_conf was;
    shot_conf_load(&was);
    for (int i = 0; i < NKEYS; i++) {
        const struct key *k = &KEYS[i];
        int v = value(c, k);
        if (v == value(&was, k)) continue;
        char s[16];
        if (k->kind == K_BOOL) snprintf(s, sizeof s, "%s", v ? "on" : "off");
        else if (k->kind == K_WORD) snprintf(s, sizeof s, "%s", k->words[v]);
        else snprintf(s, sizeof s, "%d", v);
        uconf_set(SHOT_CONF, k->name, s);
    }
    if (strcmp(c->folder, was.folder)) uconf_set(SHOT_CONF, "folder", c->folder);
    if (strcmp(c->name, was.name)) uconf_set(SHOT_CONF, "name", c->name);
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
