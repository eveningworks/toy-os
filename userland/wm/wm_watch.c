// Pushed configuration for the compositor -- see wm_watch.h.
#include "wm/wm_watch.h"
#include "wm/wm_log.h"
#include "rt/sys.h"

static const char *const PATH[WM_TOPIC_COUNT] = {
    [WM_TOPIC_SETTINGS] = 0,                    // an event, not a path
    [WM_TOPIC_ETC]      = "/etc",
    [WM_TOPIC_APPS]     = "/usr/wm/applications",
    [WM_TOPIC_DESKTOP]  = "/home/desktop",
    [WM_TOPIC_EFFECTS]  = "/etc/effects",
    [WM_TOPIC_TRASH]    = "/home/.Trash/files",
};

static int g_id[WM_TOPIC_COUNT];         // the kernel's watch id, > 0
static uint64_t g_gen[WM_TOPIC_COUNT];

void wm_watch_init(void) {
    // The bin is made on the first delete (lib/utrash.h), and a watch
    // needs a folder to watch: one refused falls back to EVERY write.
    sys_mkdir("/home/.Trash");
    sys_mkdir("/home/.Trash/files");
    sys_mkdir("/home/.Trash/info");
    for (int t = 0; t < WM_TOPIC_COUNT; t++) {
        g_gen[t] = 1;
        if (!PATH[t]) continue;
        g_id[t] = sys_fs_watch(PATH[t]);
        if (g_id[t] <= 0)
            wm_logf("wm: no watch on %s (%d) -- polling it instead\n", PATH[t], g_id[t]);
    }
}

void wm_watch_fired(int id) {
    // SEVERAL TOPICS CAN SHARE AN ID: the kernel hands back the same id
    // for the same path, and matches by hash, so bump every topic on it.
    for (int t = 0; t < WM_TOPIC_COUNT; t++)
        if (g_id[t] > 0 && g_id[t] == id) g_gen[t]++;
}

void wm_watch_setting(void) { g_gen[WM_TOPIC_SETTINGS]++; }

uint64_t wm_watch_gen(enum wm_topic t) {
    if (t < 0 || t >= WM_TOPIC_COUNT) return 0;
    if (PATH[t] && g_id[t] <= 0) return sys_fs_generation();   // the fallback
    return g_gen[t];
}
