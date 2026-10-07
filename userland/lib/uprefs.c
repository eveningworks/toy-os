#include "lib/uprefs.h"

#include <stdio.h>
#include <string.h>

#include "lib/uconf.h"

static int *ifield(void *obj, const struct upref *k) { return (int *)((char *)obj + k->off); }
static int ivalue(const void *obj, const struct upref *k) {
    return *(const int *)((const char *)obj + k->off);
}
static char *tfield(void *obj, const struct upref *k) { return (char *)obj + k->off; }
static const char *tvalue(const void *obj, const struct upref *k) {
    return (const char *)obj + k->off;
}

void uprefs_defaults(const struct uprefs *p, void *obj) {
    for (int i = 0; i < p->count; i++) {
        const struct upref *k = &p->keys[i];
        if (k->type == UPREF_TEXT) snprintf(tfield(obj, k), (size_t)k->hi, "%s", k->dflt_text ? k->dflt_text : "");
        else *ifield(obj, k) = k->dflt;
    }
}

static void parse(void *obj, const struct upref *k, const char *v) {
    switch (k->type) {
    case UPREF_BOOL:
        // The File Manager wrote `1`/`0`, and people type `yes`/`no`.
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes")) *ifield(obj, k) = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no")) *ifield(obj, k) = 0;
        break;
    case UPREF_WORD:
        for (int w = 0; w < k->hi; w++)
            if (!strcmp(v, k->words[w])) *ifield(obj, k) = w;
        break;
    case UPREF_INT: {
        int n = 0, any = 0;
        for (const char *q = v; *q >= '0' && *q <= '9' && n < 100000; q++) { n = n * 10 + (*q - '0'); any = 1; }
        if (any && n >= k->lo && n <= k->hi) *ifield(obj, k) = n;
        break;
    }
    case UPREF_TEXT:
        if ((int)strlen(v) < k->hi && (!k->valid || k->valid(v)))
            snprintf(tfield(obj, k), (size_t)k->hi, "%s", v);
        break;
    }
}

void uprefs_load(const struct uprefs *p, void *obj) {
    uprefs_defaults(p, obj);
    uprefs_read(p, obj);
}

void uprefs_read(const struct uprefs *p, void *obj) {
    static struct etc_config_buf buf;   // 4 KiB: never on a ring-3 frame
    if (!uconf_load(p->path, &buf)) return;
    for (int i = 0; i < p->count; i++) {
        char v[256];
        if (etc_config_buf_get(&buf, p->keys[i].key, v, sizeof v)) parse(obj, &p->keys[i], v);
    }
}

int uprefs_save(const struct uprefs *p, const void *obj, const void *was) {
    int ok = 1, n = 0;
    for (int i = 0; i < p->count; i++) {
        const struct upref *k = &p->keys[i];
        char s[256];
        if (k->type == UPREF_TEXT) {
            if (was && !strcmp(tvalue(obj, k), tvalue(was, k))) continue;
            snprintf(s, sizeof s, "%s", tvalue(obj, k));
        } else {
            int v = ivalue(obj, k);
            if (was && v == ivalue(was, k)) continue;
            if (k->type == UPREF_BOOL) snprintf(s, sizeof s, "%s", v ? "on" : "off");
            else if (k->type == UPREF_WORD) snprintf(s, sizeof s, "%s", v >= 0 && v < k->hi ? k->words[v] : k->words[0]);
            else snprintf(s, sizeof s, "%d", v);
        }
        if (!uconf_set(p->path, k->key, s)) ok = 0;
        else n++;
    }
    return ok ? n : -1;
}
