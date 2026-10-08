// See lib/usetting_schema.h.
//
// NOTHING IS CACHED EXCEPT THE DIRECTORY LISTING, and the split is
// deliberate: a GET or a SET names one setting, so it opens one file and
// parses it, the way the kernel's setting_text.c reads its own files
// once per lookup. What that leaves expensive is ENUMERATION -- walking
// the directory to reach index N, N times, is quadratic -- so the NAMES
// are cached and the parse is not. A name is 50 bytes; a parsed schema
// is 250.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "lib/usetting_schema.h"
#include "lib/uconf.h"
#include "lib/ushortcuts.h" // shortcut_value_valid: see uschema_validate

#define SCHEMA_PATH_MAX 192

// GROWN AS THE DIRECTORY IS READ: one machine's declarations, however
// many. A fixed table silently stopped listing at its size.
static char (*g_names)[SETTING_ABI_QUALIFIED_MAX];
static int  g_names_cap;
static int  g_name_count = -1; // -1 = never listed
static struct uschema g_cached;
static char g_cached_name[SETTING_ABI_QUALIFIED_MAX];


void uschema_invalidate(void) {
    g_name_count = -1;
    g_cached_name[0] = '\0';
}

// A file in /etc/settings.d DECLARES a setting when it carries `Type=`;
// without one it only describes a setting the kernel registered. One
// key tells them apart, so the two kinds share a directory -- and a
// group's page text (`group.<category>.<group>`) is skipped by the same
// test, having no Type= either.
static int declares(const char *qualified) {
    char path[SCHEMA_PATH_MAX];
    char type[16];
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/%s", qualified) <= 0)
        return 0;
    return uconf_get(path, USCHEMA_KEY_TYPE, type, sizeof type) && type[0];
}

static void list_names(void) {
    g_name_count = 0;
    DIR *d = opendir(SETTING_TEXT_DIR);
    if (!d) return; // no directory is a machine with no declarations
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR) continue;
        if (!strchr(e->d_name, '.')) continue; // not "<ns>.<name>"
        if (!declares(e->d_name)) continue;
        if (g_name_count == g_names_cap) {
            int cap = g_names_cap ? 2 * g_names_cap : 64;
            void *p = realloc(g_names, (size_t)cap * sizeof *g_names);
            if (!p) break;   // what fits, rather than nothing
            g_names = p;
            g_names_cap = cap;
        }
        strlcpy(g_names[g_name_count], e->d_name, sizeof g_names[0]);
        g_name_count++;
    }
    closedir(d);
}

static void ensure_names(void) { if (g_name_count < 0) list_names(); }

static uint32_t type_of(const char *word) {
    if (!strcmp(word, "enum"))     return SETTING_ABI_TYPE_ENUM;
    if (!strcmp(word, "int"))      return SETTING_ABI_TYPE_INT;
    if (!strcmp(word, "keycombo")) return SETTING_ABI_TYPE_KEYCOMBO;
    return SETTING_ABI_TYPE_STRING; // including an unrecognised word:
                                    // every value is text underneath, so
                                    // a free-text box still works
}

static int32_t int_key(const struct etc_config_buf *buf, const char *key,
                       int32_t fallback) {
    char v[16];
    if (!etc_config_buf_get(buf, key, v, sizeof v) || !v[0]) return fallback;
    char *end = 0;
    long n = strtol(v, &end, 10);
    if (end == v || (end && *end)) return fallback; // a partial number is
                                                    // a typo, not a value
    return (int32_t)n;
}

static void str_key(const struct etc_config_buf *buf, const char *key,
                    char *out, uint32_t cap) {
    if (!etc_config_buf_get(buf, key, out, cap)) out[0] = '\0';
}

// THE LAST DECLARATION PARSED, and only the DECLARATION -- never a
// value. System Settings asks INFO for a setting and then CHOICE once
// per option, all naming the same file, so without this a six-option
// dropdown parses its declaration seven times. A declaration is shipped
// metadata that changes when the machine is flashed; a VALUE is read
// from /etc every time and is never held here, which is the line that
// makes one cache entry safe.
int uschema_find(const char *qualified, struct uschema *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!qualified || !qualified[0]) return 0;

    if (g_cached_name[0] && !strcmp(g_cached_name, qualified)) {
        *out = g_cached;
        return 1;
    }

    char path[SCHEMA_PATH_MAX];
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/%s", qualified) <= 0)
        return 0;

    // HEAP: a struct etc_config_buf is the whole document and the
    // ring-3 stack is one guard page away (lib/uconf.c carries the note,
    // and tools/check_config_size.py fails the build over it).
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) return 0;
    int ok = uconf_load(path, buf);

    char word[16];
    if (!ok || !etc_config_buf_get(buf, USCHEMA_KEY_TYPE, word, sizeof word)
            || !word[0]) {
        free(buf);
        return 0; // absent, unreadable, or a text-only file
    }
    out->type = type_of(word);

    // The NAME is the file name, because the file name is the identity.
    const char *dot = strchr(qualified, '.');
    if (!dot || !dot[1]) { free(buf); return 0; }
    uint32_t ns_len = (uint32_t)(dot - qualified);
    if (ns_len >= sizeof out->ns) { free(buf); return 0; }
    memcpy(out->ns, qualified, ns_len);
    out->ns[ns_len] = '\0';
    strlcpy(out->name, dot + 1, sizeof out->name);

    str_key(buf, USCHEMA_KEY_FILE,        out->file,       sizeof out->file);
    str_key(buf, USCHEMA_KEY_LABEL,       out->label,      sizeof out->label);
    str_key(buf, USCHEMA_KEY_CATEGORY,    out->category,   sizeof out->category);
    str_key(buf, USCHEMA_KEY_GROUP,       out->group,      sizeof out->group);
    str_key(buf, USCHEMA_KEY_DEFAULT,     out->def,        sizeof out->def);
    str_key(buf, USCHEMA_KEY_UNIT,        out->unit,       sizeof out->unit);
    str_key(buf, USCHEMA_KEY_CHOICES,     out->choices,    sizeof out->choices);
    str_key(buf, USCHEMA_KEY_CHOICE_DIR,  out->choice_dir, sizeof out->choice_dir);
    {
        char req[sizeof out->req_name + sizeof out->req_value];
        str_key(buf, USCHEMA_KEY_REQUIRES, req, sizeof req);
        char *eq = strchr(req, '=');
        if (eq) {
            *eq = '\0';
            strlcpy(out->req_name, req, sizeof out->req_name);
            strlcpy(out->req_value, eq + 1, sizeof out->req_value);
        }
        str_key(buf, USCHEMA_KEY_OTHERWISE, out->otherwise, sizeof out->otherwise);
    }

    char mode[16];
    str_key(buf, USCHEMA_KEY_DIR_MODE, mode, sizeof mode);
    out->dir_mode = !strcmp(mode, "stem")   ? USCHEMA_DIR_STEM
                  : !strcmp(mode, "subdir") ? USCHEMA_DIR_SUBDIR
                                            : USCHEMA_DIR_NAME;

    out->min  = int_key(buf, USCHEMA_KEY_MIN, 0);
    out->max  = int_key(buf, USCHEMA_KEY_MAX, 0);
    out->step = int_key(buf, USCHEMA_KEY_STEP, 1);
    if (out->step <= 0) out->step = 1;

    // A LABEL IS THE FLOOR: a setting no UI can name cannot be
    // presented, so the key stands in rather than leaving a blank row.
    if (!out->label[0]) strlcpy(out->label, out->name, sizeof out->label);
    if (!out->category[0])
        strlcpy(out->category, SETTING_ABI_CATEGORY_DEFAULT, sizeof out->category);

    free(buf);
    // A DECLARATION WITH NO FILE HAS NOWHERE TO PUT THE VALUE. The
    // kernel refuses that at registration (a tunable needs an apply);
    // here there is no apply at all, so refusing is the only honest
    // answer -- the alternative is a row whose every write vanishes.
    if (!out->file[0]) return 0;
    g_cached = *out;
    strlcpy(g_cached_name, qualified, sizeof g_cached_name);
    return 1;
}

int uschema_count(void) { ensure_names(); return g_name_count; }

int uschema_at(int index, struct uschema *out) {
    ensure_names();
    if (index < 0 || index >= g_name_count) {
        if (out) memset(out, 0, sizeof *out);
        return 0;
    }
    return uschema_find(g_names[index], out);
}

// --- choices ---------------------------------------------------------

// The `index`th comma-separated field of `list`, or 0 past the end.
static int list_at(const char *list, int index, char *out, uint32_t cap) {
    if (!list || !list[0]) return 0;
    const char *p = list;
    for (int i = 0; ; i++) {
        const char *comma = strchr(p, ',');
        uint32_t len = comma ? (uint32_t)(comma - p) : (uint32_t)strlen(p);
        if (i == index) {
            if (len >= cap) len = cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return out[0] != '\0';
        }
        if (!comma) return 0;
        p = comma + 1;
    }
}

static int list_count(const char *list) {
    char scratch[SETTING_ABI_VALUE_MAX];
    int n = 0;
    while (list_at(list, n, scratch, sizeof scratch)) n++;
    return n;
}

static void put_stem(char *s) {
    char *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = '\0';
}

// Entry `index` of ONE directory, or -1 with `*count` its entries.
static int one_dir_at(const struct uschema *s, const char *dir, int index, char *out,
                      uint32_t cap, int *count) {
    DIR *d = opendir(dir);
    *count = 0;
    if (!d) return -1; // a directory with nothing in it yet is normal
    struct dirent *e;
    int seen = 0, rc = -1;
    while ((e = readdir(d))) {
        int is_dir = e->d_type == DT_DIR;
        // A CHOICE IS A NAME A VALUE CAN BE, so a mode reading files
        // skips directories and the subdir mode skips files -- the rule
        // api/setting.h's choice_dir already states for the kernel half.
        if (s->dir_mode == USCHEMA_DIR_SUBDIR ? !is_dir : is_dir) continue;
        if (seen++ != index) continue;
        strlcpy(out, e->d_name, cap);
        if (s->dir_mode == USCHEMA_DIR_STEM) put_stem(out);
        rc = out[0] != '\0';
        break;
    }
    closedir(d);
    *count = seen;
    return rc;
}

// Entry `index` across the comma-separated directories, in order.
static int dir_at(const struct uschema *s, int index, char *out, uint32_t cap) {
    const char *p = s->choice_dir;
    while (*p) {
        char dir[sizeof s->choice_dir];
        size_t n = strcspn(p, ",");
        if (n >= sizeof dir) return 0;
        memcpy(dir, p, n);
        dir[n] = '\0';
        int count;
        int rc = one_dir_at(s, dir, index, out, cap, &count);
        if (rc >= 0) return rc;
        index -= count;
        p += n;
        if (*p == ',') p++;
    }
    return 0;
}

static int dir_count(const struct uschema *s) {
    char scratch[SETTING_ABI_VALUE_MAX];
    int n = 0;
    while (dir_at(s, n, scratch, sizeof scratch)) n++;
    return n;
}

int uschema_choice(const struct uschema *s, int index, char *out, uint32_t cap) {
    if (!s || !out || !cap || index < 0) return 0;
    out[0] = '\0';
    if (s->type != SETTING_ABI_TYPE_ENUM) return 0;

    int n = list_count(s->choices);
    if (index < n) return list_at(s->choices, index, out, cap);
    index -= n;

    return dir_at(s, index, out, cap);
}

int uschema_choice_count(const struct uschema *s) {
    if (!s || s->type != SETTING_ABI_TYPE_ENUM) return 0;
    return list_count(s->choices) + dir_count(s);
}

// --- values ----------------------------------------------------------

int uschema_stored(const struct uschema *s, char *out, uint32_t cap) {
    if (out && cap) out[0] = '\0';
    if (!s || !s->file[0]) return 0;
    return uconf_get(s->file, s->name, out, cap);
}

void uschema_get(const struct uschema *s, char *out, uint32_t cap) {
    if (!out || !cap) return;
    if (!uschema_stored(s, out, cap) || !out[0]) strlcpy(out, s->def, cap);
}

int uschema_unmet(const struct uschema *s, char *reason, uint32_t cap) {
    if (reason && cap) reason[0] = '\0';
    if (!s || !s->req_name[0]) return 0;
    struct uschema other;
    if (!uschema_find(s->req_name, &other)) return 0;   // not DECLARED: ignored
    char v[SETTING_ABI_VALUE_MAX];
    uschema_get(&other, v, sizeof v);
    if (!strcmp(v, s->req_value)) return 0;
    if (reason && cap) {
        char path[SCHEMA_PATH_MAX];
        if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/%s.%s", s->ns, s->name) > 0)
            uconf_get(path, USCHEMA_KEY_REQUIRES_REASON, reason, cap);
        if (!reason[0])
            snprintf(reason, cap, "Unavailable while %s is %s", other.label, v);
    }
    return 1;
}

void uschema_effective(const struct uschema *s, char *out, uint32_t cap) {
    if (!out || !cap) return;
    if (s && s->otherwise[0] && uschema_unmet(s, 0, 0)) {
        strlcpy(out, s->otherwise, cap);
        return;
    }
    uschema_get(s, out, cap);
}

int uschema_validate(const struct uschema *s, const char *value) {
    if (!s || !value) return 0;
    if (strlen(value) >= SETTING_ABI_VALUE_MAX) return 0;

    if (s->type == SETTING_ABI_TYPE_INT) {
        if (!value[0]) return 0;
        char *end = 0;
        long v = strtol(value, &end, 10);
        if (end == value || (end && *end)) return 0;
        return v >= s->min && v <= s->max;
    }
    // A KEYCOMBO VALUE MAY BE A LIST -- "Shift+Super+S, Print Screen" is
    // one shortcut with two bindings -- so the test is the shortcuts
    // table's own, not keycombo_parse() on the whole string. One
    // implementation, compiled into both rings already.
    if (s->type == SETTING_ABI_TYPE_KEYCOMBO) return shortcut_value_valid(value);
    if (s->type == SETTING_ABI_TYPE_ENUM) {
        char scratch[SETTING_ABI_VALUE_MAX];
        int any = 0;
        for (int i = 0; uschema_choice(s, i, scratch, sizeof scratch); i++) {
            any = 1;
            if (!strcmp(scratch, value)) return 1;
        }
        // AN EMPTY LIST ACCEPTS ANYTHING, which is the kernel registry's
        // rule (choice_valid) and matters here: a wallpaper directory
        // that has not been populated yet would otherwise make every
        // value illegal rather than merely unavailable.
        return !any;
    }
    return 1; // STRING
}

int uschema_write(const struct uschema *s, const char *value, int *changed) {
    if (changed) *changed = 0;
    if (!uschema_validate(s, value)) return SETTING_INVALID;

    // SETTING IT TO WHAT IT ALREADY IS COSTS NOTHING -- and the cost is
    // not the write, it is the generation bump: everything watching it
    // re-reads /etc, reloads a cursor theme, re-parses every .desktop
    // file. A UI that applies on pointer motion then freezes the
    // machine, which is what happened before the kernel's setting_set()
    // grew the same check.
    char cur[SETTING_ABI_VALUE_MAX];
    if (uschema_stored(s, cur, sizeof cur) && !strcmp(cur, value))
        return SETTING_SAVED;

    if (!uconf_set(s->file, s->name, value)) return SETTING_UNSAVED;
    if (changed) *changed = 1;
    return SETTING_SAVED;
}

int uschema_unset(const struct uschema *s) {
    if (!s || !s->file[0]) return SETTING_INVALID;
    char cur[SETTING_ABI_VALUE_MAX];
    // Absent already: INVALID, matching the kernel's UNSET, which
    // reports "there was nothing to remove" rather than a success that
    // changed nothing.
    if (!uschema_stored(s, cur, sizeof cur) || !cur[0]) return SETTING_INVALID;
    return uconf_unset(s->file, s->name) ? SETTING_SAVED : SETTING_UNSAVED;
}

// --- presentation ----------------------------------------------------

int uschema_choice_label_for(const char *ns, const char *name,
                             const char *value, char *out, uint32_t cap) {
    if (!out || !cap || !ns || !name || !value || !value[0]) return 0;

    char path[SCHEMA_PATH_MAX], key[SETTING_ABI_VALUE_MAX + 8];
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/%s.%s", ns, name) <= 0)
        return 0;
    if (snprintf(key, sizeof key, SETTING_TEXT_CHOICE_PREFIX "%s", value) <= 0)
        return 0;
    char shown[SETTING_ABI_LABEL_MAX];
    if (!uconf_get(path, key, shown, sizeof shown) || !shown[0]) return 0;
    strlcpy(out, shown, cap);
    return 1;
}

// The schema half's caller: a declared setting has no computed label
// behind it, so the value IS the fallback and is written first.
void uschema_choice_label(const struct uschema *s, const char *value,
                          char *out, uint32_t cap) {
    if (!out || !cap) return;
    strlcpy(out, value ? value : "", cap);
    if (s) uschema_choice_label_for(s->ns, s->name, value, out, cap);
}

// A PAGE's own text, from `group.<category>.<group>`. Its own file
// rather than a field on each setting, because the text belongs to the
// group: four settings in a page would otherwise carry four copies of
// one string.
int uschema_group_text(const char *category, const char *group,
                       struct setting_msg *m) {
    if (!m) return 0;
    m->label[0] = m->description[0] = '\0';
    if (!category || !category[0] || !group || !group[0]) return 0;

    char path[SCHEMA_PATH_MAX];
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/"
                 SETTING_TEXT_GROUP_PREFIX "%s.%s", category, group) <= 0)
        return 0;
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) return 0;
    int ok = uconf_load(path, buf);
    if (ok) {
        str_key(buf, SETTING_TEXT_KEY_LABEL, m->label, sizeof m->label);
        str_key(buf, SETTING_TEXT_KEY_DESC, m->description, sizeof m->description);
    }
    free(buf);
    // A page with no text of its own is NORMAL -- it is titled by its
    // group key -- so "no file" is reported as a miss, not an error.
    return ok && (m->label[0] || m->description[0]);
}

int uschema_text_word(const char *ns, const char *name, const char *key,
                      char *out, uint32_t cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!ns || !name || !key) return 0;
    char path[SCHEMA_PATH_MAX];
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/%s.%s", ns, name) <= 0) return 0;
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) return 0;
    if (uconf_load(path, buf)) str_key(buf, key, out, cap);
    free(buf);
    return out[0] != '\0';
}

void uschema_text(const struct uschema *s, struct setting_msg *m) {
    if (s) uschema_text_for(s->ns, s->name, m);
}

void uschema_text_for(const char *ns, const char *name, struct setting_msg *m) {
    if (!m) return;
    m->description[0] = '\0';
    m->widget = SETTING_ABI_WIDGET_AUTO;
    m->sflags = 0;
    m->order  = 0;
    if (!ns || !name) return;

    char path[SCHEMA_PATH_MAX];
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/%s.%s", ns, name) <= 0)
        return;
    // ONE READ, MANY KEYS: etc_config_get() re-reads the whole document
    // per key, and this is four of them on a page a user is looking at.
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) return;
    if (uconf_load(path, buf)) {
        str_key(buf, SETTING_TEXT_KEY_DESC, m->description, sizeof m->description);

        char word[16];
        str_key(buf, SETTING_TEXT_KEY_WIDGET, word, sizeof word);
        m->widget = !strcmp(word, "radio")    ? SETTING_ABI_WIDGET_RADIO
                  : !strcmp(word, "dropdown") ? SETTING_ABI_WIDGET_DROPDOWN
                  : !strcmp(word, "slider")   ? SETTING_ABI_WIDGET_SLIDER
                  : !strcmp(word, "gallery")  ? SETTING_ABI_WIDGET_GALLERY
                                              : SETTING_ABI_WIDGET_AUTO;

        str_key(buf, SETTING_TEXT_KEY_APPLIES, word, sizeof word);
        if (!strcmp(word, "reboot")) m->sflags |= SETTING_ABI_SF_REBOOT;
        str_key(buf, SETTING_TEXT_KEY_ADVANCED, word, sizeof word);
        if (word[0] == '1') m->sflags |= SETTING_ABI_SF_ADVANCED;
        str_key(buf, SETTING_TEXT_KEY_SORT, word, sizeof word);
        if (!strcmp(word, "label")) m->sflags |= SETTING_ABI_SF_SORTED;

        m->order = int_key(buf, SETTING_TEXT_KEY_ORDER, 0);
    }
    free(buf);
}
