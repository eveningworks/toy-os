// Global keyboard shortcuts: match a key against the bound actions and
// launch one.
//
// **THE COMPOSITOR MATCHES BEFORE ROUTING, AND NOTHING CAN OPT OUT.**
// That is not a toy-os invention -- a Wayland client cannot grab a key
// at all, because one that could would be a keylogger, so the
// compositor is the only thing that ever sees a binding. A fullscreen
// app here cannot swallow Super+E for the same reason it cannot swallow
// Alt+F4 today.
//
// The bindings are DATA (kernel/lib/shortcuts_config.c), read from the
// settings registry and re-read when the filesystem generation moves --
// the same poll the wallpaper and the taskbar already use, because a
// compositor is a process and has no other way to hear about a write.
#include "wm/wm_watch.h" // wm_shortcut_poll()'s change counter
#include "wm_internal.h"
#include "wm_shortcut.h"
#include "keycombo.h"
#include "lib/ushortcuts.h"
#include "lib/usetting.h"
#include "ui/ulog.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>

// One action's parsed bindings. SEVERAL PER ACTION, because the
// screenshot tool answers to Shift+Super+S and to Print Screen -- see
// shortcuts_config.c on why a value is a list.
#define BINDINGS_PER_ACTION 4

struct bound {
    struct keycombo combo[BINDINGS_PER_ACTION];
    int count;
};

static struct bound g_bound[SHORTCUT_ACTION_MAX];
static int g_loaded;

// Split a comma-separated value and parse each combination. A binding
// that does not parse is SKIPPED WITH A LINE IN THE LOG rather than
// failing the whole value: the registry already refused a bad one on the
// way in, so anything reaching here came from a hand-edited file, and
// dropping the desktop's other shortcuts over it would be the wrong
// trade.
static void parse_value(const char *value, struct bound *out) {
    out->count = 0;
    const char *p = value;
    while (*p && out->count < BINDINGS_PER_ACTION) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - start);
        while (len > 0 && start[len - 1] == ' ') len--;
        if (len <= 0 || len >= KEYCOMBO_TEXT_MAX) continue;
        char one[KEYCOMBO_TEXT_MAX];
        memcpy(one, start, (size_t)len);
        one[len] = '\0';
        struct keycombo c;
        if (keycombo_parse(one, &c) && c.key) out->combo[out->count++] = c;
        else ulogf("wm: shortcut `%s' is not a key combination -- ignored\n", one);
    }
}

// **ITS OWN GENERATION POLL, not a ride on poll_desktop_entries().**
// That one defers while the Start menu is open, while a drag is running
// and while any window points into the app registry -- all correct for
// re-parsing .desktop entries, and all irrelevant here. Re-reading four
// strings has none of those hazards, and a shortcut that only took
// effect once the Start menu happened to be closed would be a puzzle.
//
// Costs one integer compare per frame when nothing has changed, which is
// the whole reason the counter exists (api/fs.h).
void wm_shortcut_poll(void) {
    static uint64_t seen;
    static int primed;
    uint64_t gen = wm_watch_config_gen();
    if (!primed) { primed = 1; seen = gen; return; }
    if (gen == seen) return;
    seen = gen;
    wm_shortcut_reload();
}

void wm_shortcut_reload(void) {
    int n = shortcut_action_count();
    if (n > SHORTCUT_ACTION_MAX) n = SHORTCUT_ACTION_MAX;
    for (int i = 0; i < n; i++) {
        const struct shortcut_action *a = shortcut_action_at(i);
        char qualified[64], value[SETTING_ABI_VALUE_MAX];
        snprintf(qualified, sizeof qualified, "shortcuts.%s", a->name);
        // THE FALLBACK IS THE ACTION TABLE'S, not a second copy here:
        // a machine with no /etc/shortcuts.conf still has Super+E.
        //
        // **usetting_get() RETURNS 1 FOR SUCCESS** (lib/usetting.h), not
        // 0 the way a syscall would. Testing it the other way round --
        // which is what this said first -- makes every read fall through
        // to the fallback, so a rebinding is stored, reloaded, and
        // silently ignored.
        if (!usetting_get(qualified, value, sizeof value))
            snprintf(value, sizeof value, "%s", a->fallback);
        parse_value(value, &g_bound[i]);
    }
    g_loaded = 1;
}

const char *wm_shortcut_match(int key, unsigned mods) {
    if (!g_loaded) wm_shortcut_reload();
    int n = shortcut_action_count();
    if (n > SHORTCUT_ACTION_MAX) n = SHORTCUT_ACTION_MAX;
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < g_bound[i].count; b++) {
            if (keycombo_matches(&g_bound[i].combo[b], key, (uint8_t)mods))
                return shortcut_action_at(i)->command;
        }
    }
    return 0;
}

int wm_shortcut_fire(int key, unsigned mods) {
    const char *cmd = wm_shortcut_match(key, mods);
    if (!cmd) return 0;
    // NOT WAITED FOR: this is a launch, reaped by the compositor's own
    // poll like everything else it spawns -- once it is TRACKED, which
    // it was not, and every PrtSc left a zombie.
    int pid = sys_spawn(cmd, 0, -1);
    if (pid > 0) wm_track_launched(pid);
    // THE ACTION A TEST WAITS FOR, once (docs/conventions/gui.md). It
    // names the COMMAND rather than the key, because what a test asserts
    // is that the right program started.
    ulogf("wm: shortcut -> %s (pid %d)\n", cmd, pid);
    return 1;
}

// --- the inhibitor ---------------------------------------------------
//
// ONE AT A TIME AND SCOPED TO A WINDOW, which is what makes it safe: it
// is dropped the moment that window stops being the focused one, so a
// client that crashes or wanders off mid-capture cannot leave the
// desktop with no shortcuts. See abi/win_proto.h.
static int g_inhibit_window = -1;

void wm_shortcut_inhibit(int window, int on) {
    if (on) g_inhibit_window = window;
    else if (g_inhibit_window == window) g_inhibit_window = -1;
    ulogf("wm: shortcuts %s by window %d\n", on ? "inhibited" : "released", window);
}

int wm_shortcut_inhibited(int focused_window) {
    return g_inhibit_window >= 0 && g_inhibit_window == focused_window;
}

void wm_shortcut_focus_changed(int focused_window) {
    if (g_inhibit_window >= 0 && g_inhibit_window != focused_window) {
        ulogf("wm: shortcuts released -- window %d is no longer focused\n",
              g_inhibit_window);
        g_inhibit_window = -1;
    }
}
