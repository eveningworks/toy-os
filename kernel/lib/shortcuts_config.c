// The desktop's global keyboard shortcuts, as registry settings.
//
// **THE COMPOSITOR OWNS GLOBAL SHORTCUTS, AND THAT IS NOT AN
// IMPLEMENTATION DETAIL.** A Wayland client cannot grab a key at all,
// deliberately -- one that could would be a keylogger -- so the
// compositor matches every binding before routing and no application
// gets a say. toy-os already had exactly that shape for Alt+F4 and
// Super; this makes the set DATA instead of a chain of `else if`s in
// wm.c, which is what "and management for them" needs.
//
// WHY THE REGISTRY AND NOT A FILE OF ITS OWN. A `binding = command`
// file is what KDE keeps (kglobalshortcutsrc) and it buys arbitrary
// custom commands -- but System Settings here is REGISTRY-DRIVEN, so a
// file would have needed a bespoke page while a registered setting gets
// a row, a label, a description, validation and `config set` for free.
// GNOME makes the same split and keeps its named actions in GSettings.
// The cost is real and worth stating: the ACTION LIST IS FIXED HERE.
// Binding a key to an arbitrary command means a second mechanism, and
// docs/roadmap.md carries it.
//
// **A VALUE MAY NAME SEVERAL COMBINATIONS, COMMA-SEPARATED**, because
// one action genuinely wants two keys: the screenshot tool answers to
// Shift+Super+S and to Print Screen, the way Windows binds both. That is
// GNOME's shape too -- its keybindings are ARRAYS of strings, not one.
// System Settings' capture control sets ONE, replacing the list; the
// list is for the default and for `config set`.
#include "setting.h"
#include "etc_config.h"
#include "keycombo.h"
#include "string.h"
#include "shortcuts_config.h"

#define SHORTCUTS_CONFIG_FILE "/etc/shortcuts.conf"

// --- the registry descriptors ----------------------------------------
//
// **FILLED AT REGISTER TIME FROM THE SHARED TABLE**, not written out
// here: the labels and the /etc keys belong to shortcut_actions.c, which
// the compositor reads too, and a second copy in this file would be the
// thing that drifts. What CANNOT be shared is the callbacks --
// `struct setting` holds function POINTERS and a getter has no way to
// learn which setting it was called for (api/setting.h), so there is one
// pair per action and a table of them below.
#define SHORTCUT_FNS(idx, key)                                                \
    static void key##_get(char *out, uint32_t out_size) {                     \
        const struct shortcut_action *a = shortcut_action_at(idx);            \
        if (!etc_config_get(SHORTCUTS_CONFIG_FILE, a->name, out, out_size))   \
            k_strlcpy(out, a->fallback, out_size);                            \
    }                                                                         \
    static int key##_apply(const char *value) {                               \
        if (!shortcut_value_valid(value)) return SETTING_INVALID;             \
        /* PERSIST ONLY -- there is nothing to apply in ring 0. The           \
         * compositor is a process and notices on its own generation          \
         * poll, exactly as the wallpaper and the taskbar do. */              \
        return etc_config_set(SHORTCUTS_CONFIG_FILE,                          \
                              shortcut_action_at(idx)->name, value)           \
                   ? SETTING_SAVED : SETTING_UNSAVED;                         \
    }

SHORTCUT_FNS(0, file_manager)
SHORTCUT_FNS(1, terminal)
SHORTCUT_FNS(2, screenshot)
SHORTCUT_FNS(3, task_manager)

static const struct {
    void (*get)(char *, uint32_t);
    int (*apply)(const char *);
} FNS[] = {
    { file_manager_get, file_manager_apply },
    { terminal_get,     terminal_apply },
    { screenshot_get,   screenshot_apply },
    { task_manager_get, task_manager_apply },
};
#define FN_COUNT ((int)(sizeof FNS / sizeof FNS[0]))

// THE REGISTRY KEEPS THE POINTER, so these outlive the call.
static struct setting g_settings[FN_COUNT];

void shortcuts_setting_register(void) {
    int n = shortcut_action_count();
    // A SHORTFALL IS SILENT AND BOUNDED, never a walk off the end: an
    // action added to the shared table without a callback pair here
    // simply does not appear in Settings, which is visible, rather than
    // registering a descriptor whose `get` is garbage.
    if (n > FN_COUNT) n = FN_COUNT;
    for (int i = 0; i < n; i++) {
        const struct shortcut_action *a = shortcut_action_at(i);
        g_settings[i].name     = a->name;
        g_settings[i].label    = a->label;
        g_settings[i].type     = SETTING_TYPE_KEYCOMBO;
        g_settings[i].file     = SHORTCUTS_CONFIG_FILE;
        g_settings[i].category = "Shortcuts";
        g_settings[i].group    = "Applications";
        g_settings[i].get      = FNS[i].get;
        g_settings[i].apply    = FNS[i].apply;
        setting_register(&g_settings[i]);
    }
}
