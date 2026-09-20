// See usaver.h.
#include "lib/usaver.h"
#include "lib/uconf.h"
#include "ui/ulog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ONE STATIC BUFFER, NOT A LOCAL. struct etc_config_buf is 4 KiB and a
// ring-3 frame is capped at 2 KiB (-Wframe-larger-than), so loading a
// document into a local does not build. Nothing here is re-entrant and
// nothing keeps a pointer into it past the call that filled it.
static struct etc_config_buf g_buf;

void usaver_conf_path(const char *saver, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.conf", SCREENSAVER_CONF_DIR, saver);
}

void usaver_display(const char *value, char *out, size_t cap) {
    strlcpy(out, value, cap);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}

// One `sep`-separated field out of `*p`, advancing it past the
// separator. Returns 0 at the end of the string, so a caller loops on
// it rather than counting first.
static int field(const char **p, char sep, char *out, size_t cap) {
    const char *s = *p;
    if (!*s) return 0;
    const char *e = strchr(s, sep);
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    *p = e ? e + 1 : s + strlen(s);
    return 1;
}

// `<prefix>.<key>` out of the loaded buffer, or "" -- every optional
// line in the format has this shape, and spelling the key by hand at
// each of them is how one of them ends up misspelled.
static int per_key(const char *prefix, const char *key,
                   char *out, uint32_t cap) {
    char k[USAVER_KEY_MAX + 16];
    snprintf(k, sizeof k, "%s.%s", prefix, key);
    return etc_config_buf_get(&g_buf, k, out, cap);
}

// `int:<min>..<max>:<default>` or `enum:<v1>,<v2>,...:<default>`.
// Returns 0 on anything it does not understand, and the option is then
// DROPPED rather than guessed at -- a malformed declaration showing up
// as a control with invented bounds is worse than not showing up.
static int parse_decl(const char *decl, struct usaver_opt *o) {
    if (strncmp(decl, "int:", 4) == 0) {
        const char *p = decl + 4;
        const char *dots = strstr(p, "..");
        const char *colon = strrchr(p, ':');
        if (!dots || !colon || colon < dots) return 0;
        o->type = USAVER_INT;
        o->imin = atoi(p);
        o->imax = atoi(dots + 2);
        if (o->imax <= o->imin) return 0;
        strlcpy(o->def, colon + 1, sizeof o->def);
        o->istep = 1;
        return 1;
    }
    if (strncmp(decl, "enum:", 5) == 0) {
        const char *p = decl + 5;
        // THE LAST COLON SPLITS OFF THE DEFAULT, because the choices
        // themselves are comma-separated and may not contain one.
        const char *colon = strrchr(p, ':');
        if (!colon) return 0;
        o->type = USAVER_ENUM;
        strlcpy(o->def, colon + 1, sizeof o->def);
        char list[USAVER_CHOICE_MAX * USAVER_VALUE_MAX];
        size_t n = (size_t)(colon - p);
        if (n >= sizeof list) return 0;
        memcpy(list, p, n);
        list[n] = '\0';
        const char *q = list;
        while (o->choice_count < USAVER_CHOICE_MAX &&
               field(&q, ',', o->choice[o->choice_count],
                     sizeof o->choice[0]))
            if (o->choice[o->choice_count][0]) o->choice_count++;
        return o->choice_count > 0;
    }
    return 0;
}

// Is `v` a value this option can actually hold? A conf file is ordinary
// text that `edit` can change, so this is the only thing standing
// between a typo and a saver reading a number outside its own range.
static int allows(const struct usaver_opt *o, const char *v) {
    if (!v[0]) return 0;
    if (o->type == USAVER_INT) {
        for (const char *p = v; *p; p++)
            if (*p < '0' || *p > '9') return 0;
        int n = atoi(v);
        return n >= o->imin && n <= o->imax;
    }
    for (int i = 0; i < o->choice_count; i++)
        if (strcmp(o->choice[i], v) == 0) return 1;
    return 0;
}

static void parse_one(const char *key, struct usaver_opt *o) {
    char v[USAVER_CHOICE_MAX * USAVER_VALUE_MAX];

    memset(o, 0, sizeof *o);
    strlcpy(o->key, key, sizeof o->key);

    if (!per_key("Option", key, v, sizeof v) || !parse_decl(v, o)) {
        o->key[0] = '\0';   // dropped -- the caller skips it
        return;
    }

    if (per_key("Label", key, v, sizeof v) && v[0])
        strlcpy(o->label, v, sizeof o->label);
    else
        usaver_display(key, o->label, sizeof o->label);

    if (per_key("Desc", key, v, sizeof v)) strlcpy(o->desc, v, sizeof o->desc);

    if (o->type == USAVER_INT) {
        if (per_key("Step", key, v, sizeof v) && atoi(v) > 0) o->istep = atoi(v);
        if (per_key("Unit", key, v, sizeof v)) strlcpy(o->unit, v, sizeof o->unit);
    } else {
        if (per_key("Widget", key, v, sizeof v)) {
            if (strcmp(v, "radio") == 0)         o->widget = USAVER_WIDGET_RADIO;
            else if (strcmp(v, "dropdown") == 0) o->widget = USAVER_WIDGET_DROPDOWN;
            else if (strcmp(v, "slider") == 0)   o->widget = USAVER_WIDGET_SLIDER;
        }
    }

    strlcpy(o->value, o->def, sizeof o->value);
}

// THE GENERIC HALF, because a saver stopped being the only thing with
// declared options: a window EFFECT has them too (lib/ueffect.h), and
// the two differ in nothing but where their files live. Copying this
// for the second owner would have been two parsers over one format.
//
// The TYPE keeps its `usaver` name. Renaming it touches 77 call sites
// across five savers for no behaviour change, and the header says what
// it really is.
int usaver_load_files(const char *name, const char *desc_path,
                      const char *conf_path, struct usaver *out) {
    char keys[USAVER_OPT_MAX * USAVER_KEY_MAX + 8];

    memset(out, 0, sizeof *out);
    strlcpy(out->name, name, sizeof out->name);

    if (!uconf_load(desc_path, &g_buf)) return 0;

    // THE ORDER OF THE PAGE IS THIS LINE. The parser has no way to
    // enumerate the keys of a document, and hand-walking the buffer for
    // `Option.*` would be a second parser beside the shared one -- so
    // the descriptor names its options once, in the order they should
    // be shown.
    if (!etc_config_buf_get(&g_buf, "Options", keys, sizeof keys)) return 1;

    const char *p = keys;
    char key[USAVER_KEY_MAX];
    while (out->opt_count < USAVER_OPT_MAX && field(&p, ',', key, sizeof key)) {
        if (!key[0]) continue;
        parse_one(key, &out->opt[out->opt_count]);
        if (out->opt[out->opt_count].key[0]) out->opt_count++;
    }
    if (!out->opt_count) return 1;

    // The saved values, over the defaults already in place. A file that
    // is not there yet is the normal state -- nothing has been changed.
    if (uconf_load(conf_path, &g_buf)) {
        for (int i = 0; i < out->opt_count; i++) {
            char v[USAVER_VALUE_MAX];
            struct usaver_opt *o = &out->opt[i];
            if (etc_config_buf_get(&g_buf, o->key, v, sizeof v) && allows(o, v))
                strlcpy(o->value, v, sizeof o->value);
        }
    }

    return 1;
}

// A SAVER's options: the generic loader over the two saver paths.
int usaver_load(const char *saver, struct usaver *out) {
    char desc[96], conf[96];
    snprintf(desc, sizeof desc, "%s/%s.saver", SCREENSAVER_DESC_DIR, saver);
    usaver_conf_path(saver, conf, sizeof conf);
    if (!usaver_load_files(saver, desc, conf, out)) return 0;

    // ONE LINE SAYING WHAT IT RESOLVED TO, here rather than in each
    // saver. It is the only way to answer "did my option take" on a
    // machine where reading pixels is not an option -- a bare-metal
    // laptop -- and it distinguishes a value that was REFUSED (the
    // default appears) from one that was never written.
    //
    // **IN THIS WRAPPER, NOT IN THE LOADER**: a saver start is once a
    // screen blank, while the compositor re-reads an EFFECT's options
    // whenever the filesystem changes -- which put three of these lines
    // in the log in two seconds, and the klog ring holds a few hundred.
    char line[160];
    unsigned n = 0;
    n += (unsigned)snprintf(line, sizeof line, "opts %s:", saver);
    for (int i = 0; i < out->opt_count && n < sizeof line - 1; i++)
        n += (unsigned)snprintf(line + n, sizeof line - n, " %s=%s",
                                out->opt[i].key, out->opt[i].value);
    ulogf("%s\n", line);
    return 1;
}

const struct usaver_opt *usaver_find(const struct usaver *s, const char *key) {
    for (int i = 0; i < s->opt_count; i++)
        if (strcmp(s->opt[i].key, key) == 0) return &s->opt[i];
    return 0;
}

int usaver_int(const struct usaver *s, const char *key, int fallback) {
    const struct usaver_opt *o = usaver_find(s, key);
    return o ? atoi(o->value) : fallback;
}

const char *usaver_str(const struct usaver *s, const char *key,
                       const char *fallback) {
    const struct usaver_opt *o = usaver_find(s, key);
    return o ? o->value : fallback;
}

int usaver_index(const struct usaver *s, const char *key, int fallback) {
    const struct usaver_opt *o = usaver_find(s, key);
    if (!o) return fallback;
    for (int i = 0; i < o->choice_count; i++)
        if (strcmp(o->choice[i], o->value) == 0) return i;
    return fallback;
}
