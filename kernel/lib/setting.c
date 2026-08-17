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
#include "font_config.h"
#include "cursor_config.h"
#include "keyboard_config.h"
#include "cursor_theme_config.h"
#include "setting_abi.h"
#include "config_file.h"
#include "fs.h"

static const struct setting *g_settings[SETTING_MAX];
static int g_count = 0;
static uint32_t g_generation = 0;

int setting_register(const struct setting *s) {
    if (!s || !s->name || !*s->name || !s->label || !s->get) return 0;
    // An ENUM with no way to list its options would render as an empty
    // picker -- a control that draws and cannot be used, which this
    // project has shipped before. Refuse at registration instead.
    if (s->type == SETTING_TYPE_ENUM && !s->choice) return 0;
    if (k_strlen(s->name) >= SETTING_NAME_MAX) return 0;
    if (k_strlen(s->label) >= SETTING_LABEL_MAX) return 0;
    if (g_count >= SETTING_MAX) {
        klog_write("setting: registry full, refusing ");
        klog_write(s->name);
        klog_write("\n");
        return 0;
    }
    // A duplicate name is refused rather than shadowing: which copy won
    // would depend on boot order, and the loser would still appear in
    // every enumeration.
    if (setting_find(s->name)) return 0;
    g_settings[g_count++] = s;
    return 1;
}

int setting_unregister(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_count; i++) {
        if (k_strcmp(g_settings[i]->name, name) != 0) continue;
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

const struct setting *setting_find(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_count; i++) {
        if (k_strcmp(g_settings[i]->name, name) == 0) return g_settings[i];
    }
    return 0;
}

int setting_get(const char *name, char *out, uint32_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    const struct setting *s = setting_find(name);
    if (!s) return 0;
    s->get(out, out_size);
    return 1;
}

enum setting_result setting_set(const char *name, const char *value) {
    const struct setting *s = setting_find(name);
    if (!s || !value) return SETTING_INVALID;
    if (k_strlen(value) >= SETTING_VALUE_MAX) return SETTING_INVALID;

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
        r = etc_config_set(s->file, s->name, value) ? SETTING_SAVED : SETTING_UNSAVED;
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

// How many options an ENUM setting offers. Walked rather than stored,
// because a choice list can be COMPUTED (the keyboard layouts are a
// directory listing), so a cached count would go stale the moment a
// layout file appeared.
static int choice_count(const struct setting *s) {
    if (s->type != SETTING_TYPE_ENUM || !s->choice) return 0;
    char scratch[SETTING_VALUE_MAX];
    int n = 0;
    while (s->choice(n, scratch, sizeof scratch)) n++;
    return n;
}

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
        msg->type = (s->type == SETTING_TYPE_ENUM) ? SETTING_ABI_TYPE_ENUM
                                                   : SETTING_ABI_TYPE_STRING;
        msg->count = choice_count(s);
        msg->value[0] = '\0';
        s->get(msg->value, sizeof msg->value);
        // What the file says, which is NOT always what is in effect --
        // see `stored` in setting_abi.h. Left empty when the key is
        // absent, meaning the setting is at its built-in default.
        msg->stored[0] = '\0';
        etc_config_get(s->file, s->name, msg->stored, sizeof msg->stored);
        return 1;
    }

    case SETTING_OP_CHOICE: {
        const struct setting *s = setting_at(msg->index);
        if (!s || s->type != SETTING_TYPE_ENUM || !s->choice) return 0;
        msg->value[0] = '\0';
        if (!s->choice(msg->choice, msg->value, sizeof msg->value)) return 0;
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
        msg->count = fs_exists(f->path) ? 1 : 0;
        msg->value[0] = '\0';
        return 1;
    }

    case SETTING_OP_UNSET: {
        msg->name[sizeof msg->name - 1] = '\0';
        const struct setting *s = setting_find(msg->name);
        if (!s) { msg->result = SETTING_INVALID; return 1; }
        msg->result = etc_config_unset(s->file, s->name) ? SETTING_SAVED
                                                         : SETTING_INVALID;
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
    font_config_setting_register();
    cursor_config_setting_register();
    keyboard_config_setting_register();
    cursor_theme_setting_register();
    config_files_scan();
}
