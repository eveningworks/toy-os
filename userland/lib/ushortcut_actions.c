// The bindable actions, and what counts as a legal binding.
//
// **RING 3 ONLY, AND IT USED TO BE COMPILED TWICE.** The kernel half
// (`shortcuts_config.c`) registered four settings so System Settings
// could draw them, and needed this table to do it; the shortcuts are
// declared by files in /etc/settings.d now, so nothing in ring 0 reads
// this and the shared-source rule it lived under is gone.
//
// What the table still is: the one place naming each action's COMMAND,
// read by the compositor when a binding fires. A binding naming a
// program nothing launches would be a shortcut that silently does
// nothing, which is why the command and the /etc key stay together.
#include "keycombo.h"
#include "string.h"
#include "lib/ushortcuts.h"

#define SHORTCUTS_CONFIG_FILE "/etc/shortcuts.conf"

// One row per bindable action: the key in the file, what a UI calls it,
// what it launches, and the factory binding.
//
// THE COMMAND IS DECLARED HERE, beside the binding, rather than resolved
// through the .desktop entries: this table is read on the KEY PATH, and
// a directory scan per keystroke to answer a question whose answer never
// changes would be the wrong trade.
//
// **SO IT HAS TO AGREE WITH data/wm/applications/, AND NOTHING CHECKS
// THAT.** Two of the four were wrong the first time this ran -- the
// Terminal's binary is `uterm`, not `terminal`, and Task Manager lives
// under wm/system rather than wm/apps -- and the symptom was a silent
// `pid -1` in the log with no window. `tools/shortcut_test.py` compares
// the two lists now, which is the check that would have caught it.
static const struct shortcut_action ACTIONS[] = {
    { "file_manager", "File Manager",  "Open the file manager",
      "/bin/wm/apps/files",      "Super+E" },
    { "terminal",     "Terminal",      "Open a terminal window",
      "/bin/wm/apps/uterm",      "Ctrl+Alt+T" },
    { "screenshot",   "Screenshot",    "Open the screenshot tool",
      "/bin/wm/apps/screenshot", "Shift+Super+S, Print Screen" },
    { "task_manager", "Task Manager",  "Open the task manager",
      "/bin/wm/system/taskmgr",  "Ctrl+Alt+Delete" },
    // ^ NOT Windows' Ctrl+Shift+Esc, and not by preference: this
    // keyboard driver DROPS Ctrl with anything that is not a letter
    // (kernel/drivers/input/keyboard.c), and Esc comes off the layout
    // rather than the special-key table, so Ctrl+Shift+Esc can never
    // reach anyone. Delete is pushed before the Ctrl fold, so it can.
};
#define ACTION_COUNT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

int shortcut_action_count(void) { return ACTION_COUNT; }

const struct shortcut_action *shortcut_action_at(int i) {
    if (i < 0 || i >= ACTION_COUNT) return 0;
    return &ACTIONS[i];
}

// Every combination in a comma-separated value must parse, and the value
// as a whole must not be longer than the settings ABI can carry. An
// EMPTY value is legal and means unbound -- that is how a shortcut is
// switched off, and refusing it would leave no way to do so.
int shortcut_value_valid(const char *value) {
    if (!value) return 0;
    const char *p = value;
    int any = 0;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - start);
        while (len > 0 && (start[len - 1] == ' ')) len--;
        if (len <= 0) continue;
        if (len >= KEYCOMBO_TEXT_MAX) return 0;
        char one[KEYCOMBO_TEXT_MAX];
        k_memcpy(one, start, (size_t)len);
        one[len] = '\0';
        struct keycombo c;
        if (!keycombo_parse(one, &c) || !c.key) return 0;
        any = 1;
    }
    (void)any;   // nothing found at all is "unbound", which is fine
    return 1;
}

