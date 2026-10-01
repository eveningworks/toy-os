// See start_store.h.
#include "start_store.h"
#include "wm_conf.h"
#include "wm_log.h"
#include "gui_apps.h"
#include "kapi.h"

#define START_CONF "/etc/start-menu.conf"

// One row per app the machine has ever launched. Bounded by the
// registry's own cap: an id that is not in the registry any more is
// still worth keeping in the FILE (an app may come back), but nothing
// in memory needs to outnumber what can be shown.
#define STORE_MAX GUI_APP_MAX

struct run_row {
    char id[GUI_APP_ICON_MAX];   // AppId shares that bound -- see gui_apps.h
    uint32_t count;
    uint32_t seq;
};

static struct run_row g_runs[STORE_MAX];
static int g_run_count;

static char g_pins[STORE_MAX][GUI_APP_ICON_MAX];
static int g_pin_count;

static uint32_t g_seq;     // the newest sequence number handed out
static int g_loaded;

// --- the file ---------------------------------------------------------

// `pinned` is a WORD LIST rather than one key per pin, because the
// ORDER is the value: a list keeps it in one place, where `pin.3=x`
// would encode it as index numbers that have to be renumbered whenever
// something in the middle is removed.
static void parse_pins(const char *raw) {
    g_pin_count = 0;
    const char *p = raw;
    while (*p && g_pin_count < STORE_MAX) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        int n = 0;
        char *out = g_pins[g_pin_count];
        while (*p && *p != ' ' && *p != ',' && n < (int)sizeof g_pins[0] - 1)
            out[n++] = *p++;
        out[n] = '\0';
        while (*p && *p != ' ' && *p != ',') p++;   // an over-long id is truncated, not split
        if (n) g_pin_count++;
    }
}

static struct run_row *row_for(const char *app_id, int create) {
    if (!app_id || !app_id[0]) return 0;
    for (int i = 0; i < g_run_count; i++)
        if (k_strcmp(g_runs[i].id, app_id) == 0) return &g_runs[i];
    if (!create || g_run_count >= STORE_MAX) return 0;
    struct run_row *r = &g_runs[g_run_count++];
    k_strlcpy(r->id, app_id, sizeof r->id);
    r->count = 0;
    r->seq = 0;
    return r;
}

// `run.<id> = <count> <seq>`. Two numbers on one line rather than two
// keys: they are written together, always, and a row that had one
// without the other would be a state this code would then have to have
// an opinion about.
static void parse_run(const char *key, const char *value) {
    struct run_row *r = row_for(key, 1);
    if (!r) return;
    uint32_t count = 0, seq = 0;
    const char *p = value;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') count = count * 10 + (uint32_t)(*p++ - '0');
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') seq = seq * 10 + (uint32_t)(*p++ - '0');
    r->count = count;
    r->seq = seq;
    if (seq > g_seq) g_seq = seq;
}

void start_store_load(void) {
    g_run_count = 0;
    g_pin_count = 0;
    g_seq = 0;
    g_loaded = 1;

    // ONE read, many questions -- etc_config_get() re-reads the whole
    // document per key, and this file has a row per app.
    static struct etc_config_buf cfg;
    if (!wm_conf_load(START_CONF, &cfg)) return;   // no file yet is the normal first boot

    char raw[STORE_MAX * GUI_APP_ICON_MAX];
    if (etc_config_buf_get(&cfg, "pinned", raw, sizeof raw)) parse_pins(raw);

    // ASKED PER INSTALLED APP, because the config parser answers for a
    // key and does not enumerate them. What that costs is history for
    // an app that is not installed RIGHT NOW: it stays in the file --
    // wm_conf_set() preserves every line it was not asked about -- and
    // comes back with the app, it is simply not in memory meanwhile.
    // Which is the correct behaviour anyway: the menu can only list
    // apps that exist.
    char key[GUI_APP_ICON_MAX + 8], value[32];
    for (int i = 0; i < gui_app_registry_count; i++) {
        const char *id = gui_app_registry[i].app_id;
        if (!id || !id[0]) continue;
        k_snprintf(key, sizeof key, "run.%s", id);
        if (!etc_config_buf_get(&cfg, key, value, sizeof value)) continue;
        parse_run(id, value);
    }
}

static void write_pins(void) {
    char list[STORE_MAX * GUI_APP_ICON_MAX];
    list[0] = '\0';
    uint32_t used = 0;
    for (int i = 0; i < g_pin_count; i++) {
        uint32_t n = k_strlen(g_pins[i]);
        if (used + n + 2 >= sizeof list) break;
        if (used) list[used++] = ' ';
        k_strlcpy(list + used, g_pins[i], sizeof list - used);
        used += n;
    }
    list[used] = '\0';
    if (!wm_conf_set(START_CONF, "pinned", list))
        wm_logf("wm: could not write %s\n", START_CONF);
}

// --- favourites -------------------------------------------------------

int start_store_pin_index(const char *app_id) {
    if (!g_loaded) start_store_load();
    if (!app_id) return -1;
    for (int i = 0; i < g_pin_count; i++)
        if (k_strcmp(g_pins[i], app_id) == 0) return i;
    return -1;
}

int start_store_is_pinned(const char *app_id) {
    return start_store_pin_index(app_id) >= 0;
}

int start_store_pin_count(void) {
    if (!g_loaded) start_store_load();
    return g_pin_count;
}

const char *start_store_pin_at(int n) {
    if (!g_loaded) start_store_load();
    if (n < 0 || n >= g_pin_count) return 0;
    return g_pins[n];
}

void start_store_pin(const char *app_id) {
    if (!app_id || !app_id[0]) return;
    if (start_store_is_pinned(app_id)) return;
    if (g_pin_count >= STORE_MAX) return;
    k_strlcpy(g_pins[g_pin_count++], app_id, sizeof g_pins[0]);
    write_pins();
}

void start_store_unpin(const char *app_id) {
    int i = start_store_pin_index(app_id);
    if (i < 0) return;
    for (int j = i; j < g_pin_count - 1; j++)
        k_strlcpy(g_pins[j], g_pins[j + 1], sizeof g_pins[0]);
    g_pin_count--;
    write_pins();
}

// --- launches ---------------------------------------------------------

void start_store_record_launch(const char *app_id) {
    if (!g_loaded) start_store_load();
    struct run_row *r = row_for(app_id, 1);
    if (!r) return;
    r->count++;
    r->seq = ++g_seq;

    char key[GUI_APP_ICON_MAX + 8], value[32];
    k_snprintf(key, sizeof key, "run.%s", r->id);
    k_snprintf(value, sizeof value, "%u %u", r->count, r->seq);
    if (!wm_conf_set(START_CONF, key, value))
        wm_logf("wm: could not record %s in %s\n", r->id, START_CONF);
}

uint32_t start_store_launch_count(const char *app_id) {
    if (!g_loaded) start_store_load();
    struct run_row *r = row_for(app_id, 0);
    return r ? r->count : 0;
}

uint32_t start_store_last_seq(const char *app_id) {
    if (!g_loaded) start_store_load();
    struct run_row *r = row_for(app_id, 0);
    return r ? r->seq : 0;
}

void start_store_forget_launches(void) {
    if (!g_loaded) start_store_load();
    if (!g_run_count) return;
    char key[GUI_APP_ICON_MAX + 8];
    for (int i = 0; i < g_run_count; i++) {
        k_snprintf(key, sizeof key, "run.%s", g_runs[i].id);
        wm_conf_unset(START_CONF, key);
    }
    g_run_count = 0;
    g_seq = 0;
}
