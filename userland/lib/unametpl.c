// See unametpl.h.
#include "lib/unametpl.h"
#include <string.h>

static int is_sep(char c) { return c == '-' || c == '_' || c == ' ' || c == '.'; }

static const struct unametpl_var *find(const struct unametpl_var *v, int n, const char *name, int len) {
    for (int i = 0; i < n; i++)
        if (v[i].name && (int)strlen(v[i].name) == len && !strncmp(v[i].name, name, (size_t)len))
            return &v[i];
    return 0;
}

int unametpl_expand(const char *tpl, const struct unametpl_var *vars, int n, char *out, int cap) {
    if (!tpl || !out || cap <= 1) return 0;
    int o = 0;
    for (const char *p = tpl; *p; ) {
        if (*p == '/') goto bad;
        if (*p != '<') {
            if (o >= cap - 1) goto bad;
            out[o++] = *p++;
            continue;
        }
        const char *e = strchr(p + 1, '>');
        if (!e) goto bad;
        const struct unametpl_var *v = find(vars, n, p + 1, (int)(e - p - 1));
        if (!v) goto bad;
        const char *val = v->value ? v->value : "";
        if (!*val) {
            // The separator BEFORE goes when one is there; else the one
            // after, so "<window>-shot" with no window is "shot".
            if (o > 0 && is_sep(out[o - 1])) o--;
            else if (o == 0 && is_sep(e[1])) e++;
        }
        for (; *val; val++) {
            if (*val == '/' || o >= cap - 1) goto bad;
            out[o++] = *val;
        }
        p = e + 1;
    }
    out[o] = '\0';
    if (o == 0) goto bad;
    return 1;
bad:
    out[0] = '\0';
    return 0;
}

int unametpl_valid(const char *tpl, const struct unametpl_var *vars, int n) {
    // Every token filled with a stand-in: what fails here fails for any
    // values, short of a value too long for the caller's buffer.
    struct unametpl_var probe[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) { probe[i].name = vars[i].name; probe[i].value = "x"; }
    char out[256];
    return unametpl_expand(tpl, probe, n, out, sizeof out);
}

void unametpl_clean(const char *in, char *out, int cap) {
    if (!out || cap <= 0) return;
    int o = 0, gap = 0;
    for (const char *p = in ? in : ""; *p && o < cap - 1; p++) {
        char c = *p;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
        if (!keep || c == '-') { gap = 1; continue; }
        if (gap && o > 0) {
            if (o >= cap - 2) break;
            out[o++] = '-';
        }
        gap = 0;
        out[o++] = c;
    }
    out[o] = '\0';
}
