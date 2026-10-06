// Renaming many files at once -- see urename.h.
#include "lib/urename.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "rt/sys.h"
#include "kpath.h"

// Where the extension starts ("dusk.jpg" -> 4), or the length when there
// is none. A leading dot is a name, not an extension (".profile").
static int ext_at(const char *name) {
    const char *dot = strrchr(name, '.');
    return (dot && dot != name) ? (int)(dot - name) : (int)strlen(name);
}

static int number(const struct urename_rule *r, int i, char *out, int cap) {
    const char *p = r->pattern;
    const char *run = strchr(p, '#');
    long n = (long)r->start + i;
    if (!run) return snprintf(out, (size_t)cap, "%s %ld", p, n) < cap;
    int k = 0;
    while (run[k] == '#') k++;
    int width = k > r->digits ? k : r->digits;
    return snprintf(out, (size_t)cap, "%.*s%0*ld%s", (int)(run - p), p, width, n, run + k) < cap;
}

static int replace(const struct urename_rule *r, const char *in, char *out, int cap) {
    size_t fl = strlen(r->find), o = 0;
    if (!fl) return (int)strlcpy(out, in, (size_t)cap) < cap;
    for (const char *s = in; *s; ) {
        int hit = r->match_case ? !strncmp(s, r->find, fl) : !strncasecmp(s, r->find, fl);
        const char *add = hit ? r->replace : s;
        size_t al = hit ? strlen(r->replace) : 1;
        if (o + al >= (size_t)cap) return 0;
        memcpy(out + o, add, al);
        o += al;
        s += hit ? fl : 1;
    }
    out[o] = '\0';
    return 1;
}

static void recase(int casing, char *s) {
    int word = 1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (casing == URENAME_LOWER) *s = (char)tolower(c);
        else if (casing == URENAME_UPPER) *s = (char)toupper(c);
        else {
            *s = (char)(word ? toupper(c) : tolower(c));
            word = !isalnum(c);
        }
    }
}

int urename_name(const struct urename_rule *r, const char *name, int i, char *out, int cap) {
    char stem[URENAME_NAME], made[URENAME_NAME];
    int e = r->keep_ext ? ext_at(name) : (int)strlen(name);
    if (e >= (int)sizeof stem) return 0;
    memcpy(stem, name, (size_t)e);
    stem[e] = '\0';
    const char *ext = r->keep_ext ? name + e : "";

    int ok;
    if (r->mode == URENAME_NUMBER) ok = number(r, i, made, sizeof made);
    else if (r->mode == URENAME_REPLACE) ok = replace(r, stem, made, sizeof made);
    else { ok = (int)strlcpy(made, stem, sizeof made) < (int)sizeof made; recase(r->casing, made); }
    if (!ok || !made[0] || strchr(made, '/')) return 0;
    if (snprintf(out, (size_t)cap, "%s%s", made, ext) >= cap) return 0;
    return strcmp(out, ".") && strcmp(out, "..");
}

int urename_plan(const struct urename_rule *r, const char *dir, char (*names)[URENAME_NAME],
                 int n, char (*news)[URENAME_NAME], int *status) {
    int changes = 0;
    for (int i = 0; i < n; i++) {
        if (!urename_name(r, names[i], i, news[i], URENAME_NAME)) {
            news[i][0] = '\0';
            status[i] = URENAME_BAD;
            continue;
        }
        status[i] = strcmp(news[i], names[i]) ? URENAME_OK : URENAME_SAME;
    }
    for (int i = 0; i < n; i++) {
        if (status[i] != URENAME_OK) continue;
        // Two of the set to one name.
        for (int j = 0; j < n; j++)
            if (j != i && status[j] != URENAME_BAD && !strcmp(news[i], news[j]))
                status[i] = URENAME_CLASH;
        if (status[i] != URENAME_OK) continue;
        // Something already there that is not itself being renamed away.
        int renamed_away = 0;
        for (int j = 0; j < n; j++)
            if (!strcmp(names[j], news[i]) && status[j] == URENAME_OK) renamed_away = 1;
        char path[512];
        struct sys_stat st;
        if (!renamed_away && k_path_join(dir, news[i], path, sizeof path) && sys_stat(path, &st) == 0)
            status[i] = URENAME_CLASH;
    }
    for (int i = 0; i < n; i++) if (status[i] == URENAME_OK) changes++;
    return changes;
}
