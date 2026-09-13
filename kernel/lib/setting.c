// The settings registry -- see setting.h for what it is and why it
// exists. This file is only the table and the dispatch; everything that
// knows what a setting MEANS lives with the subsystem that owns it.
//
// The registry stores POINTERS (see setting.h's trap): registration is
// once per boot from static descriptors, so copying them would buy
// nothing and cost the ability to have a `choice` list computed at call
// time.
#include "setting.h"
#include "string.h"
#include "klog.h"
#include "tz.h"
#include "ntp_config.h"
#include "shell_config.h"
#include "font_config.h"
#include "cursor_config.h"
#include "mouse_config.h"
#include "sound_config.h"
#include "display_config.h"
#include "setting_text.h"
#include "keyboard_config.h"
#include "cursor_theme_config.h"
#include "wallpaper_config.h"
#include "start_button_config.h"
#include "taskbar_config.h"
#include "tray_config.h"
#include "screensaver_config.h"
#include "week_start_config.h"
#include "icon_size_config.h"
#include "window_drag_config.h"
#include "target.h"
#include "storage_config.h"
#include "conn_log.h"
#include "setting_abi.h"
#include "config_file.h"
#include "fs.h"

// Defined below, beside the three choice sources it asks about.
static int setting_has_choices(const struct setting *s);

static const struct setting *g_settings[SETTING_MAX];
static int g_count = 0;
static uint32_t g_generation = 0;

int setting_register(const struct setting *s) {
    if (!s || !s->name || !*s->name || !s->label || !s->get) return 0;
    // An ENUM with no way to list its options would render as an empty
    // picker -- a control that draws and cannot be used, which this
    // project has shipped before. Refuse at registration instead.
    if (s->type == SETTING_TYPE_ENUM && !setting_has_choices(s)) return 0;
    // An INT with no usable range would accept everything, which is a
    // STRING wearing the wrong type -- and every UI reading imin/imax
    // would lay out a control with no ends. Refused at registration, so
    // the mistake is a boot-time absence rather than a widget that
    // behaves oddly much later.
    if (s->type == SETTING_TYPE_INT && s->min >= s->max) return 0;
    // A TUNABLE WITH NO `apply` HOLDS ITS VALUE NOWHERE. The
    // persisted-only flavour (apply == NULL) works because the registry
    // writes the file itself; with no file there is nothing to write
    // and nothing to apply, so a `set` would report success and change
    // nothing observable. Refused here rather than discovered later.
    if (!setting_persists(s) && !s->apply) return 0;
    if (k_strlen(s->name) >= SETTING_NAME_MAX) return 0;
    if (k_strlen(s->label) >= SETTING_LABEL_MAX) return 0;
    if (g_count >= SETTING_MAX) {
        klog_write("setting: registry full, refusing ");
        klog_write(s->name);
        klog_write("\n");
        return 0;
    }
    // A duplicate is refused rather than shadowing -- which copy won
    // would depend on boot order, and the loser would still appear in
    // every enumeration. What counts as a duplicate is the PAIR
    // (namespace, name): two programs may both own a `theme`, as long
    // as they persist it to different files. Same name in the same file
    // is a genuine collision and always was.
    for (int i = 0; i < g_count; i++) {
        if (k_strcmp(g_settings[i]->name, s->name) != 0) continue;
        const char *a = g_settings[i]->file, *b = s->file;
        if (a == b || (a && b && k_strcmp(a, b) == 0)) {
            klog_write("setting: refusing duplicate ");
            klog_write(s->name);
            klog_write(" in the same file\n");
            return 0;
        }
    }
    g_settings[g_count++] = s;
    return 1;
}

int setting_unregister(const char *name) {
    if (!name) return 0;
    // Through setting_find(), so removing uses exactly the identity
    // rule looking up does -- including refusing an ambiguous bare
    // name, where guessing would silently remove the wrong one.
    const struct setting *want = setting_find(name);
    if (!want) return 0;
    for (int i = 0; i < g_count; i++) {
        if (g_settings[i] != want) continue;
        // Close the gap rather than leaving a hole: `setting_at()` is
        // indexed by row number, and a NULL in the middle would make a
        // UI's enumeration stop early at it.
        for (int j = i; j < g_count - 1; j++) g_settings[j] = g_settings[j + 1];
        g_count--;
        return 1;
    }
    return 0;
}

int setting_count(void) { return g_count; }

const struct setting *setting_at(int index) {
    if (index < 0 || index >= g_count) return 0;
    return g_settings[index];
}

int setting_persists(const struct setting *s) {
    // A setting with no file at all, and one in the RUNTIME namespace,
    // are the same answer: nothing to write. Kept as one predicate so
    // the five etc_config_* sites below cannot each decide differently
    // -- which is exactly how a tunable would end up half-persisted,
    // written by `set` and never read back at boot.
    if (!s || !s->file || !s->file[0]) return 0;
    return k_strcmp(s->file, CONFIG_PATH_RUNTIME) != 0;
}

const char *setting_namespace(const struct setting *s) {
    if (!s || !s->file) return "";
    const struct config_file *f = config_file_find_by_path(s->file);
    return f ? f->name : "";
}

int setting_qualified(const struct setting *s, char *out, uint32_t out_size) {
    if (!s || !out || out_size == 0) return 0;
    const char *ns = setting_namespace(s);
    // k_strlcpy/k_strlcat return the length they TRIED to create, so
    // `>= out_size` is the truncation test -- and a truncated qualified
    // name is a different setting's name, not a shortened one.
    out[0] = '\0';
    if (*ns) {
        if (k_strlcpy(out, ns, out_size) >= out_size) return 0;
        if (k_strlcat(out, ".", out_size) >= out_size) return 0;
    }
    return k_strlcat(out, s->name, out_size) < out_size;
}

// Splits "ns.name" into its halves. Returns 1 when a dot was found, 0
// when the input is a bare name -- so a caller can tell "the user
// qualified this" from "the user did not", which is the difference
// between an exact match and one that may be ambiguous.
static int split_qualified(const char *in, char *ns, uint32_t ns_size,
                           const char **bare) {
    const char *dot = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '.') { dot = p; break; }
    }
    if (!dot) { *bare = in; if (ns_size) ns[0] = '\0'; return 0; }
    uint32_t n = (uint32_t)(dot - in);
    if (n >= ns_size) n = ns_size - 1;
    for (uint32_t i = 0; i < n; i++) ns[i] = in[i];
    ns[n] = '\0';
    *bare = dot + 1;
    return 1;
}

// The one matcher both setting_find() and setting_matches() use, so
// "which setting does this name mean" has a single definition.
// `want_ns` is "" for a bare name.
static int match(const struct setting *s, const char *want_ns, const char *bare) {
    if (k_strcmp(s->name, bare) != 0) return 0;
    if (!*want_ns) return 1;
    return k_strcmp(setting_namespace(s), want_ns) == 0;
}

int setting_matches(const char *name) {
    if (!name || !*name) return 0;
    char ns[CONFIG_NAME_MAX];
    const char *bare = name;
    split_qualified(name, ns, sizeof ns, &bare);
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (match(g_settings[i], ns, bare)) n++;
    }
    return n;
}

const struct setting *setting_find(const char *name) {
    if (!name || !*name) return 0;
    char ns[CONFIG_NAME_MAX];
    const char *bare = name;
    split_qualified(name, ns, sizeof ns, &bare);

    const struct setting *hit = 0;
    for (int i = 0; i < g_count; i++) {
        if (!match(g_settings[i], ns, bare)) continue;
        // AMBIGUOUS: refuse rather than return the first. Returning one
        // would make which setting a bare name means depend on
        // registration order, i.e. on boot sequence.
        if (hit) return 0;
        hit = g_settings[i];
    }
    return hit;
}

int setting_get(const char *name, char *out, uint32_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    const struct setting *s = setting_find(name);
    if (!s) return 0;
    s->get(out, out_size);
    return 1;
}

// An INT setting's value, validated against its own bounds. Returns 1
// and writes the parsed number to `out`, or 0 for anything that is not
// a number in range.
//
// **THE REGISTRY IS THE GATE, NOT THE UI.** A spinbox knows the range
// and will not offer a value outside it, but `config set
// system.mouse_speed 0` and a hand-edited /etc/toyos.conf reach this
// same function without passing through any widget -- and one of those
// used to be able to freeze the pointer, which is the reason
// mouse_config.c made speed an ENUM of named levels in the first place.
// Checking here is what let that workaround go.
//
// REFUSED, not clamped. A caller that asked for 500 and silently got
// 300 has been told its request succeeded, and the next thing it reads
// back disagrees with what it sent; `config` then prints a value the
// user did not type. An out-of-range value is a mistake worth
// reporting.
static int int_value_ok(const struct setting *s, const char *value, int *out) {
    if (!value || !value[0]) return 0;
    int neg = (*value == '-');
    const char *p = neg ? value + 1 : value;
    if (!*p) return 0;
    int32_t v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 1000000) return 0; // no overflow, and no plausible setting
    }
    if (neg) v = -v;
    if (v < s->min || v > s->max) return 0;
    if (out) *out = (int)v;
    return 1;
}

const char *setting_unavailable(const struct setting *s) {
    if (!s || !s->unavailable) return 0;
    const char *why = s->unavailable();
    // An empty string is the same answer as NULL, so a callback may
    // return either without a client having to test for both.
    return (why && why[0]) ? why : 0;
}

// --- a choice list that is a FILE (setting.h's `choice_file`) --------
//
// READ ONCE AND CACHED. Enumerating is O(choices) by construction --
// System Settings asks for all 92 timezone rows to fill one dropdown --
// and a whole-file read per row is the shape that made a nine-entry
// desktop reload cost 54 of them (api/etc_config.h). The cache is keyed
// on the path AND the filesystem generation, so an edit is picked up
// without anything having to remember to invalidate.
#define CHOICE_FILE_MAX 8192
static char cf_buf[CHOICE_FILE_MAX];
static uint32_t cf_len;
static const char *cf_path;       // compared by POINTER: every caller
static uint64_t cf_gen;           // passes a string literal from a
static int cf_valid;              // `struct setting`, which outlives us

static int choice_file_load(const char *path) {
    uint64_t gen = fs_generation();
    if (cf_valid && cf_path == path && cf_gen == gen) return 1;
    cf_valid = 0;
    cf_path = path;
    cf_gen = gen;
    // fs_read_into() REFUSES a file larger than the buffer rather than
    // truncating it, which is the answer we want: half a city list is a
    // list that is silently missing cities.
    uint32_t n = fs_read_into(path, cf_buf, sizeof cf_buf - 1);
    if (!n) return 0;
    cf_buf[n] = '\0';
    cf_len = n;
    cf_valid = 1;
    return 1;
}

// Field `want` of line `index`: 0 is the value, -1 the LAST field,
// which is the label. Returns 1, or 0 past the last line.
static int choice_file_field(const char *path, int index, int want,
                             char *out, uint32_t out_size) {
    if (!out || !out_size || !choice_file_load(path)) return 0;
    uint32_t i = 0;
    int line = 0;
    while (i < cf_len) {
        uint32_t start = i;
        while (i < cf_len && cf_buf[i] != '\n') i++;
        uint32_t end = i;
        if (i < cf_len) i++;
        if (end > start && cf_buf[end - 1] == '\r') end--;
        if (end == start) continue;            // a blank line is not a choice
        if (line++ != index) continue;
        // The field: walk the commas within [start, end).
        uint32_t fs = start, fe = end, seen = 0;
        if (want == 0) {
            fe = start;
            while (fe < end && cf_buf[fe] != ',') fe++;
        } else {
            for (uint32_t p = start; p < end; p++)
                if (cf_buf[p] == ',') { fs = p + 1; seen++; }
            if (!seen) fs = start;             // one field: it is both
        }
        uint32_t len = fe - fs;
        if (len >= out_size) len = out_size - 1;
        k_memcpy(out, cf_buf + fs, len);
        out[len] = '\0';
        return 1;
    }
    return 0;
}

// A CHOICE LIST THAT IS A DIRECTORY (setting.h's `choice_dir`).
//
// NOT cached, where choice_file is: fs_list() walks in the backend and
// the whole answer is one name, so there is no buffer to keep warm. The
// cost is a walk per index, which is why the 92-row timezone list is a
// FILE -- a directory of savers or themes is single digits.
static int cd_want, cd_seen;
static char cd_found[SETTING_VALUE_MAX];

static void choice_dir_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (is_dir) return;
    if (cd_seen == cd_want) k_strlcpy(cd_found, name, sizeof cd_found);
    cd_seen++;
}

static int choice_dir_at(const char *dir, int index, char *out, uint32_t out_size) {
    cd_want = index;
    cd_seen = 0;
    cd_found[0] = '\0';
    fs_list(dir, choice_dir_cb);
    if (!cd_found[0]) return 0;
    k_strlcpy(out, cd_found, out_size);
    return 1;
}

// The `index`-th choice of an ENUM setting, from whichever source it
// declared. One place, so the four callers cannot disagree about which
// settings have a choice list.
static int setting_choice_at(const struct setting *s, int index,
                             char *out, uint32_t out_size) {
    if (!s || s->type != SETTING_TYPE_ENUM) return 0;
    if (s->choice) return s->choice(index, out, out_size);
    if (s->choice_file)
        return choice_file_field(s->choice_file, index, 0, out, out_size);
    if (s->choice_dir) return choice_dir_at(s->choice_dir, index, out, out_size);
    return 0;
}

static int setting_has_choices(const struct setting *s) {
    return s && s->type == SETTING_TYPE_ENUM
           && (s->choice || s->choice_file || s->choice_dir);
}

// Whether `value` is one of the setting's choices.
//
// AN EMPTY LIST ACCEPTS ANYTHING, which is not the same answer as "no
// value is valid". A list is data -- a directory of themes, a database
// file -- and it can be empty because the data is missing, at which
// point refusing every value would make the setting impossible to put
// BACK. That is a lockout, and a missing /etc/timezones would have
// caused it.
static int choice_valid(const struct setting *s, const char *value) {
    char scratch[SETTING_VALUE_MAX];
    int any = 0;
    for (int i = 0; setting_choice_at(s, i, scratch, sizeof scratch); i++) {
        any = 1;
        if (k_strcmp(scratch, value) == 0) return 1;
    }
    return !any;
}

// How many options an ENUM setting offers. Walked rather than stored,
// because a choice list can be COMPUTED (the keyboard layouts are a
// directory listing), so a cached count would go stale the moment a
// layout file appeared.
static int choice_count(const struct setting *s) {
    if (!setting_has_choices(s)) return 0;
    char scratch[SETTING_VALUE_MAX];
    int n = 0;
    while (setting_choice_at(s, n, scratch, sizeof scratch)) n++;
    return n;
}

enum setting_result setting_set(const char *name, const char *value) {
    const struct setting *s = setting_find(name);
    if (!s || !value) return SETTING_INVALID;
    if (k_strlen(value) >= SETTING_VALUE_MAX) return SETTING_INVALID;
    // BEFORE anything is applied or written. The REGISTRY is the gate,
    // not the UI: `config set` and a hand-edited /etc file reach this
    // same function without passing through any control, so a setting
    // that cannot take effect here must be refused in one place rather
    // than in each client. See api/setting.h's `unavailable`.
    if (setting_unavailable(s)) return SETTING_INVALID;
    // BEFORE the already-correct check below, so an out-of-range value
    // is refused rather than being quietly accepted when it happens to
    // equal what is already there.
    if (s->type == SETTING_TYPE_INT && !int_value_ok(s, value, 0))
        return SETTING_INVALID;
    // AN ENUM'S VALUE MUST BE ONE OF ITS CHOICES, checked here for the
    // same reason the range is: `config set` and a hand-edited /etc file
    // reach this function without passing through any control. It was
    // each setting's own `apply` that refused an unknown value, which
    // worked only for the ones that remembered to.
    if (setting_has_choices(s) && !choice_valid(s, value))
        return SETTING_INVALID;

    // SETTING IT TO WHAT IT ALREADY IS COSTS NOTHING. Without this,
    // re-selecting the current value writes the file and bumps the
    // generation, and everything watching the generation does real work
    // -- the desktop re-reads and re-parses every .desktop file, a
    // compositor reloads its cursor theme. A UI that over-reports a
    // change then freezes the machine, which is exactly what happened:
    // Control Panel applied a setting on every pointer-motion event and
    // the resulting churn was seconds long.
    //
    // The file is checked too, not just the live value: on a disk where
    // the key is absent (a fresh image, or one where /etc was lost) the
    // live value already matches and skipping would leave nothing
    // written, so "already correct" has to mean correct in BOTH places.
    char cur[SETTING_VALUE_MAX];
    cur[0] = '\0';
    if (s->get) s->get(cur, sizeof cur);
    if (cur[0] && k_strcmp(cur, value) == 0) {
        // A TUNABLE HAS NO SECOND PLACE TO CHECK, so the live value
        // matching IS the whole answer -- there is no file that could
        // disagree with it. Falling through to etc_config_get() here
        // would ask for a key in "(runtime)" and, finding nothing,
        // re-apply on every set of an unchanged value.
        if (!setting_persists(s)) return SETTING_SAVED;
        char on_disk[SETTING_VALUE_MAX];
        if (etc_config_get(s->file, s->name, on_disk, sizeof on_disk) &&
            k_strcmp(on_disk, value) == 0) {
            return SETTING_SAVED; // nothing to do, and nothing to announce
        }
    }

    enum setting_result r;
    if (s->apply) {
        r = (enum setting_result)s->apply(value);
    } else {
        // Persisted-only: nothing in this kernel applies it, so the
        // registry does the one thing it can do honestly -- write it
        // down and bump the generation so its real owner notices.
        // A runtime setting with no apply would be a value nothing
        // holds -- neither applied nor stored -- so it is refused
        // rather than silently accepted. Registration rejects it too;
        // this is the belt to that braces.
        r = !setting_persists(s) ? SETTING_INVALID
          : (etc_config_set(s->file, s->name, value) ? SETTING_SAVED : SETTING_UNSAVED);
    }
    // The generation tracks APPLIED, not SAVED: a value that took effect
    // in memory but failed to persist is still a change a cache holder
    // has to see. SETTING_INVALID changed nothing.
    if (r != SETTING_INVALID) g_generation++;
    return r;
}

uint32_t setting_generation(void) { return g_generation; }

int settings_reload(void) {
    // Settings persist as ORDINARY TEXT FILES, on purpose -- /etc is
    // editable with `edit` and readable with `cat`, and the registry is
    // an index over those files rather than a replacement for them. The
    // cost of that choice is exactly this function: a hand-edited file
    // and the subsystem's live in-memory copy disagree until someone
    // re-reads it, and the next `set` would otherwise write the stale
    // in-memory value straight back over the edit.
    //
    // Re-applies through each setting's own `apply`, so a value edited
    // to something illegal is REFUSED here (leaving the working value
    // in place) rather than taken on trust -- and the count of refusals
    // is what the caller reports, since silently ignoring a typo in a
    // file someone just edited is how a setting appears not to work.
    int rejected = 0;
    char value[SETTING_VALUE_MAX];
    for (int i = 0; i < g_count; i++) {
        const struct setting *s = g_settings[i];
        if (!s->apply) continue; // persisted-only: no live copy to refresh
        // A TUNABLE IS NOT RELOADED, and that is the point of it: there
        // is no file to have been edited, and re-applying would mean
        // inventing a value. `config reload` leaves it exactly as it is.
        if (!setting_persists(s)) continue;
        // Nothing this machine can do with it -- and applying anyway
        // would count a refusal against the FILE, which is not what is
        // wrong. See api/setting.h's `unavailable`.
        if (setting_unavailable(s)) continue;
        if (!etc_config_get(s->file, s->name, value, sizeof value)) continue;
        if (!value[0]) continue;
        if (s->apply(value) == SETTING_INVALID) rejected++;
    }
    // Re-read /etc/config.d too, so `config register` followed by
    // `config reload` picks up a new descriptor with no restart -- the
    // same live-reload property the desktop's `.desktop` entries have.
    config_files_scan();
    // Bumped unconditionally: a reload's whole premise is that the
    // files may have changed behind us, so a cache holder has to
    // re-read whether or not any single apply reported a change.
    g_generation++;
    return rejected;
}

// --- the ring-3 face of the registry (abi/setting_abi.h) -------------

int setting_dispatch(struct setting_msg *msg) {
    if (!msg) return 0;

    // Every reply carries the generation, whatever the op, so a client
    // learns "someone changed something" from a call it was making
    // anyway rather than needing a poll of its own.
    msg->generation = g_generation;

    switch (msg->op) {
    case SETTING_OP_COUNT:
        msg->count = g_count;
        return 1;

    case SETTING_OP_INFO: {
        const struct setting *s = setting_at(msg->index);
        if (!s) return 0;
        k_strlcpy(msg->name, s->name, sizeof msg->name);
        k_strlcpy(msg->label, s->label, sizeof msg->label);
        k_strlcpy(msg->file, s->file ? s->file : "", sizeof msg->file);
        k_strlcpy(msg->ns, setting_namespace(s), sizeof msg->ns);
        // NULL becomes the default here, at the boundary, rather than
        // in each client: a UI that had to know the fallback string
        // would be a second place it is written down.
        k_strlcpy(msg->category, s->category ? s->category : SETTING_CATEGORY_DEFAULT,
                  sizeof msg->category);
        // Empty rather than substituted: "no group" is a real answer a
        // UI acts on (give it a page of its own), unlike "no category",
        // where every setting must land somewhere.
        k_strlcpy(msg->group, s->group ? s->group : SETTING_GROUP_DEFAULT,
                  sizeof msg->group);
        // The text half, from /etc/settings.d -- absent is normal, and
        // leaves the description empty and the widget AUTO.
        setting_text_description(setting_namespace(s), s->name,
                                 msg->description, sizeof msg->description);
        msg->widget = setting_text_widget(setting_namespace(s), s->name);
        // Why this one cannot be changed here, or empty. From the
        // registry rather than /etc: it describes the MACHINE.
        k_strlcpy(msg->unavailable, setting_unavailable(s) ? setting_unavailable(s) : "",
                  sizeof msg->unavailable);
        msg->sflags = setting_text_sflags(setting_namespace(s), s->name);
        msg->order  = setting_text_order(setting_namespace(s), s->name);
        msg->type = s->type == SETTING_TYPE_ENUM ? SETTING_ABI_TYPE_ENUM
                  : s->type == SETTING_TYPE_INT  ? SETTING_ABI_TYPE_INT
                                                 : SETTING_ABI_TYPE_STRING;
        // Zero on every other type, so a client that ignores them sees
        // nothing new -- and a client that reads them on an ENUM gets an
        // empty range rather than a plausible wrong one.
        msg->imin = msg->imax = msg->istep = 0;
        msg->unit[0] = '\0';
        if (s->type == SETTING_TYPE_INT) {
            msg->imin  = s->min;
            msg->imax  = s->max;
            msg->istep = s->step > 0 ? s->step : 1;
            k_strlcpy(msg->unit, s->unit ? s->unit : "", sizeof msg->unit);
        }
        msg->count = choice_count(s);
        msg->value[0] = '\0';
        s->get(msg->value, sizeof msg->value);
        // What the file says, which is NOT always what is in effect --
        // see `stored` in setting_abi.h. Left empty when the key is
        // absent, meaning the setting is at its built-in default.
        msg->stored[0] = '\0';
        // A tunable has no stored form at all, so `stored` stays empty
        // -- which is what a client already renders as "at its default,
        // nothing written". That reads correctly here for a different
        // reason (there is nowhere to write), and is why this needs no
        // new ABI field to say so.
        if (setting_persists(s)) {
            etc_config_get(s->file, s->name, msg->stored, sizeof msg->stored);
        }
        return 1;
    }

    case SETTING_OP_CHOICE: {
        const struct setting *s = setting_at(msg->index);
        if (!setting_has_choices(s)) return 0;
        msg->value[0] = '\0';
        if (!setting_choice_at(s, msg->choice, msg->value, sizeof msg->value))
            return 0;
        // The DISPLAY name rides alongside the value -- so a client
        // draws `label` unconditionally and never decides. `value`
        // stays the token that gets stored.
        //
        // THREE SOURCES, MOST SPECIFIC FIRST: /etc/settings.d, because
        // that is where an installation renames or translates one
        // choice; then the setting's own `choice_label`, for a list
        // computed from data no file could enumerate; then the value
        // itself, which is always presentable if not always pretty.
        if (!setting_text_choice(setting_namespace(s), s->name, msg->value,
                                 msg->label, sizeof msg->label)) {
            // setting_text_choice() has already written the value into
            // `label` as its own fallback, so a choice_label that
            // declines leaves exactly what it would have left.
            if (s->choice_label)
                s->choice_label(msg->choice, msg->label, sizeof msg->label);
            else if (s->choice_file)
                choice_file_field(s->choice_file, msg->choice, -1,
                                  msg->label, sizeof msg->label);
        }
        return 1;
    }

    case SETTING_OP_GET:
        msg->name[sizeof msg->name - 1] = '\0'; // it came from userland
        return setting_get(msg->name, msg->value, sizeof msg->value);

    case SETTING_OP_SET:
        msg->name[sizeof msg->name - 1] = '\0';
        msg->value[sizeof msg->value - 1] = '\0';
        msg->result = (uint32_t)setting_set(msg->name, msg->value);
        // The generation may have just moved -- re-read it, or the
        // caller that made the change is told the OLD one and concludes
        // nothing happened.
        msg->generation = g_generation;
        // Not a failure: an unregistered name or a rejected value is a
        // legitimate answer, and it is in `result`. Returning 0 here
        // would make a client unable to tell "the ABI is wrong" from
        // "you typed a bad value".
        return 1;

    case SETTING_OP_GROUP_TEXT: {
        // "<category>/<group>" in `name`, split on the first slash --
        // neither half may contain one, and a category is a UI section
        // name rather than a path.
        char cat[SETTING_ABI_CATEGORY_MAX];
        const char *slash = 0;
        for (const char *p = msg->name; *p; p++)
            if (*p == '/') { slash = p; break; }
        if (!slash) return 0;
        uint32_t n = (uint32_t)(slash - msg->name);
        if (n >= sizeof cat) return 0;
        for (uint32_t i = 0; i < n; i++) cat[i] = msg->name[i];
        cat[n] = '\0';
        msg->label[0] = '\0';
        msg->description[0] = '\0';
        return setting_text_group(cat, slash + 1,
                                  msg->label, sizeof msg->label,
                                  msg->description, sizeof msg->description);
    }

    case SETTING_OP_FILE_COUNT:
        msg->count = config_file_count();
        return 1;

    case SETTING_OP_FILE_INFO: {
        const struct config_file *f = config_file_at(msg->index);
        if (!f) return 0;
        k_strlcpy(msg->name, f->name, sizeof msg->name);
        k_strlcpy(msg->file, f->path, sizeof msg->file);
        k_strlcpy(msg->label, f->desc, sizeof msg->label);
        msg->type = (uint32_t)f->builtin;
        // Whether the file is actually THERE. A descriptor may name a
        // file a program has not written yet, and filtering those out
        // would leave "where do my settings go?" unanswerable until
        // after the first save -- so it is reported, not hidden.
        // The runtime namespace has no file to be there, so it reports
        // present rather than "missing" -- a sentinel stat'd as a path
        // would say the kernel's own tunables are not installed.
        msg->count = config_file_is_runtime(f) ? 1 : (fs_exists(f->path) ? 1 : 0);
        msg->value[0] = '\0';
        return 1;
    }

    case SETTING_OP_UNSET: {
        msg->name[sizeof msg->name - 1] = '\0';
        const struct setting *s = setting_find(msg->name);
        if (!s) { msg->result = SETTING_INVALID; return 1; }
        // `unset` means "forget what was written and fall back to the
        // default". A tunable has nothing written, so there is nothing
        // to forget -- refused rather than reported as done, since a
        // caller expecting the value to revert would be misled.
        msg->result = !setting_persists(s) ? SETTING_INVALID
                    : (etc_config_unset(s->file, s->name) ? SETTING_SAVED
                                                          : SETTING_INVALID);
        // The generation moves because what is STORED changed, even
        // though nothing applied -- a client showing "modified on disk"
        // has to notice.
        if (msg->result == SETTING_SAVED) msg->generation = ++g_generation;
        return 1;
    }

    case SETTING_OP_RELOAD:
        msg->count = settings_reload();
        msg->generation = g_generation;
        return 1;

    default:
        return 0;
    }
}

void settings_init(void) {
    tz_setting_register();
    ntp_setting_register();
    shell_setting_register();
    conn_log_setting_register();
    font_config_setting_register();
    cursor_config_setting_register();
    mouse_config_setting_register();
    sound_config_setting_register();
    display_config_setting_register();
    keyboard_config_setting_register();
    cursor_theme_setting_register();
    wallpaper_setting_register();
    start_button_setting_register();
    taskbar_setting_register();
    tray_setting_register();
    screensaver_setting_register();
    week_start_setting_register();
    icon_size_setting_register();
    window_drag_setting_register();
    storage_config_setting_register();
    target_setting_register();
    tunables_register();
    config_files_scan();
}
