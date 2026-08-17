#include "gui_apps.h"
#include "kapi.h"
#include "etc_config.h"
#include "rt/sys.h"
#include "wm/wm_fs.h"
#include "wm/wm_log.h"

// The Start menu and the desktop icons are built from DATA ON DISK --
// one file per app in /usr/wm/desktop/, scanned at desktop startup.
// Adding an app to the desktop is dropping a file there; it is not
// editing this table and rebuilding the kernel.
//
// The format is `name=value` with `#` comments, read through
// kernel/lib/etc_config.c -- the same parser /etc/toyos.conf uses,
// because a second config format to maintain would buy nothing. It is a
// deliberate small subset of freedesktop.org's .desktop files: same
// idea, same key names where they overlap, none of the localisation or
// MIME machinery that has nothing to attach to here. See
// data/wm/desktop/README.md.
//
// TWO KINDS OF ENTRY, and the reason the file format has to know:
//
//   Exec=/bin/wm/apps/calculator   spawn that binary; the process makes
//                                  its own window over TWP.
//
// There is no second form any more. `Exec=builtin:<name>` named a
// kernel-space app whose callbacks were compiled in; it was removed
// when Control Panel -- the last one -- became a ring-3 binary
// (Milestone 41 stage 4's prerequisite), which is exactly what the
// format was shaped to allow without any reader of it changing. An
// entry still carrying `builtin:` now names no binary and is refused
// by the loader below like any other bad path.

#define DESKTOP_DIR "/usr/wm/desktop"

// The live registry. Not const any more: it is filled in at startup from
// the directory above. Everything that reads it (start_menu.c,
// desktop.c, wm.c, wm_debug.c) is unchanged -- they still see a flat
// array and a count.
struct gui_app gui_app_registry[GUI_APP_MAX];
int gui_app_registry_count;

// Storage for the strings the entries point at. A desktop entry's Name
// and Exec come out of a file, so they need somewhere to live for the
// session; there is no allocator worth using here and the counts are
// tiny, so it is a fixed table like everything else in this WM.
static char g_names[GUI_APP_MAX][GUI_APP_NAME_MAX];
static char g_execs[GUI_APP_MAX][GUI_APP_EXEC_MAX];
static char g_cats[GUI_APP_MAX][16];

// Scanning state: fs_list()'s callback carries no context pointer, so
// the walk collects filenames here first and parses afterwards. Parsing
// inside the callback would mean reading files while a directory walk
// is in progress, which is a re-entrancy the filesystem does not
// promise.
static char g_files[GUI_APP_MAX][64];
static int g_file_count;

static void collect(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (is_dir || g_file_count >= GUI_APP_MAX) return;
    // Only *.desktop, so a README or an editor's leftover is ignored
    // rather than parsed into a blank menu row.
    int n = (int)k_strlen(name);
    if (n < 9 || k_strcmp(name + n - 8, ".desktop") != 0) return;
    k_strlcpy(g_files[g_file_count], name, sizeof g_files[0]);
    g_file_count++;
}

// Category order in the menu: the desktop's own things first, then real
// apps, then the demos. An unknown category sorts last rather than
// being dropped -- a typo should show up as a misplaced row, not as an
// app that silently vanished.
static int cat_rank(const char *c) {
    if (k_strcmp(c, "system") == 0) return 0;
    if (k_strcmp(c, "apps") == 0) return 1;
    if (k_strcmp(c, "demos") == 0) return 2;
    return 3;
}

// An entry's ShowIn= key -> GUI_SHOW_* bits. Absent means BOTH, which is
// what every entry meant before this key existed.
//
// Words are separated by spaces or commas: `ShowIn=desktop startmenu`.
//
// A key naming nothing recognisable falls back to BOTH and says so,
// rather than rejecting the way this project's parsers normally do. The
// usual rule ("a parser rejects rather than guesses") assumes the two
// outcomes are a value and an error; here the failure directions are
// asymmetric. Hiding an app because its ShowIn= was misspelled makes it
// vanish from the desktop with no visible cause, which is the single
// worst thing this file can do -- an unreachable app looks like a broken
// system (see wm_reap_launched()'s comment for the last time that
// happened). Showing it in both places with a log line is recoverable.
static unsigned parse_show_in(const struct etc_config_buf *cfg, const char *file) {
    char raw[32];
    if (!etc_config_buf_get(cfg, "ShowIn", raw, sizeof raw)) return GUI_SHOW_ALL;

    unsigned bits = 0;
    const char *p = raw;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        const char *word = p;
        while (*p && *p != ' ' && *p != ',') p++;
        int len = (int)(p - word);

        if (len == 7 && k_strncmp(word, "desktop", 7) == 0) {
            bits |= GUI_SHOW_DESKTOP;
        } else if (len == 9 && k_strncmp(word, "startmenu", 9) == 0) {
            bits |= GUI_SHOW_STARTMENU;
        } else {
            wm_logf("wm: %s has an unknown ShowIn word -- ignored\n", file);
        }
    }

    if (!bits) {
        wm_logf("wm: %s names no valid ShowIn surface -- showing on both\n", file);
        return GUI_SHOW_ALL;
    }
    return bits;
}

static void load_entry(const char *file) {
    char path[64];
    k_snprintf(path, sizeof path, "%s/%s", DESKTOP_DIR, file);

    // ONE read, six questions. This used to be six etc_config_get()
    // calls, each of which re-reads the whole file -- so a nine-entry
    // reload cost 54 whole-file reads, ran on every filesystem change
    // (sys_fs_generation() is global, so saving an unrelated setting
    // triggers it), and froze the desktop for 2.5s under KVM, where
    // each port-I/O instruction is a VM exit. It measured 40ms under
    // TCG, which is why it went unnoticed: every test here runs TCG.
    struct etc_config_buf cfg;
    if (!etc_config_load(path, &cfg)) return;

    char name[GUI_APP_NAME_MAX], exec[GUI_APP_EXEC_MAX];
    char cat[16], icon[8], nodisplay[8];
    if (!etc_config_buf_get(&cfg, "Name", name, sizeof name)) return;
    if (!etc_config_buf_get(&cfg, "Exec", exec, sizeof exec)) return;
    if (!etc_config_buf_get(&cfg, "Category", cat, sizeof cat)) k_strlcpy(cat, "apps", sizeof cat);
    if (!etc_config_buf_get(&cfg, "Icon", icon, sizeof icon)) icon[0] = '\0';
    if (etc_config_buf_get(&cfg, "NoDisplay", nodisplay, sizeof nodisplay)
        && nodisplay[0] == '1') return;

    int i = gui_app_registry_count;
    if (i >= GUI_APP_MAX) return;

    k_strlcpy(g_names[i], name, GUI_APP_NAME_MAX);
    k_strlcpy(g_execs[i], exec, GUI_APP_EXEC_MAX);
    k_strlcpy(g_cats[i], cat, sizeof g_cats[0]);

    struct gui_app *a = &gui_app_registry[i];
    k_memset(a, 0, sizeof *a);
    a->name = g_names[i];
    a->icon = icon[0];
    a->resizable = 1;
    a->show_in = parse_show_in(&cfg, file);

    // Every app is a ring-3 binary now. An entry still naming the
    // retired `builtin:` form is refused LOUDLY rather than shown as a
    // menu row that does nothing when clicked -- the same treatment an
    // unknown builtin always got, kept because an old entry file can
    // outlive the mechanism it named.
    if (k_strncmp(g_execs[i], "builtin:", 8) == 0) {
        wm_logf("wm: %s uses the retired builtin: form -- ignored\n", file);
        return;
    }

    a->exec_path = g_execs[i];
    gui_app_registry_count++;
}

// A cheap answer to "could anything in this directory have changed?",
// costing ONE directory listing and no per-file reads.
//
// The desktop reloads when sys_fs_generation() moves, and that counter is
// GLOBAL -- it bumps for any write anywhere, so saving a setting made
// the desktop re-read and re-parse every .desktop file. Under KVM each
// of those reads competes with whatever ring-3 app just did the saving,
// and the reload measured 2.5 SECONDS with the desktop frozen for all
// of it. Almost every one of those reloads had nothing to reload: the
// directory was byte-for-byte what it already was.
//
// WHAT IT CANNOT SEE: an edit that leaves a file's SIZE unchanged, and
// leaves the set of names unchanged -- fs_list() reports names and
// sizes, and reading each file to do better is the cost this exists to
// avoid. The live reload is a convenience (the alternative was no live
// reload at all), so missing a same-size edit until the next real
// change is the right trade against freezing the desktop on every
// unrelated write. fs_stat()'s mtime would close it at the price of a
// stat per entry, which is the same shape of cost again.
static uint64_t g_fp;

static void fingerprint_cb(const char *name, uint32_t size, int is_dir) {
    if (is_dir) return;
    // FNV-1a over the name, then fold in the size. Order-independent
    // addition would collide on a swap; this is order-DEPENDENT, which
    // is fine because fs_list() walks in stable table order.
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = name; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)size * 2654435761ULL;
    g_fp = (g_fp * 31) + h;
}

uint64_t gui_apps_dir_fingerprint(void) {
    g_fp = 1;
    struct dirent ents[GUI_APP_MAX];
    int n = wm_fs_list(DESKTOP_DIR, ents, GUI_APP_MAX);
    for (int i = 0; i < n; i++) fingerprint_cb(ents[i].name, ents[i].size, (int)ents[i].is_dir);
    return g_fp;
}

void gui_apps_load(void) {
    gui_app_registry_count = 0;
    g_file_count = 0;

    struct dirent ents[GUI_APP_MAX];
    int n = wm_fs_list(DESKTOP_DIR, ents, GUI_APP_MAX);
    for (int i = 0; i < n; i++) collect(ents[i].name, ents[i].size, (int)ents[i].is_dir);

    // Filename order is whatever the directory hands back, so sort by
    // (category, filename) before parsing: the Start menu's row order is
    // something tests and users both read, and "whatever order the
    // filesystem felt like" is not an order.
    for (int i = 0; i < g_file_count; i++) {
        uint64_t t = sys_ticks();
        load_entry(g_files[i]);
        uint64_t d = sys_ticks() - t;
        if (d * 10 >= 100)
            wm_logf("wm: entry %s took %u ms\n", g_files[i], (uint32_t)(d * 10));
    }

    for (int i = 1; i < gui_app_registry_count; i++) {
        for (int j = i; j > 0; j--) {
            int rj = cat_rank(g_cats[j]), rp = cat_rank(g_cats[j - 1]);
            if (rj > rp || (rj == rp && k_strcmp(g_names[j], g_names[j - 1]) >= 0)) break;
            struct gui_app ta = gui_app_registry[j];
            gui_app_registry[j] = gui_app_registry[j - 1];
            gui_app_registry[j - 1] = ta;
            char tmp[GUI_APP_NAME_MAX];
            k_strlcpy(tmp, g_names[j], sizeof tmp);
            k_strlcpy(g_names[j], g_names[j - 1], GUI_APP_NAME_MAX);
            k_strlcpy(g_names[j - 1], tmp, GUI_APP_NAME_MAX);
            char tc[16];
            k_strlcpy(tc, g_cats[j], sizeof tc);
            k_strlcpy(g_cats[j], g_cats[j - 1], sizeof g_cats[0]);
            k_strlcpy(g_cats[j - 1], tc, sizeof g_cats[0]);
            // The registry entries point INTO g_names, so the swapped
            // rows have to be re-pointed rather than carrying stale
            // pointers to each other's storage.
            gui_app_registry[j].name = g_names[j];
            gui_app_registry[j - 1].name = g_names[j - 1];
        }
    }

    if (gui_app_registry_count == 0) {
        // No entries: a disk without /usr/wm, or a RAM-only boot with
        // nothing seeded. Say so -- an empty Start menu with no
        // explanation looks like the desktop is broken, and this is the
        // one message that distinguishes "no apps installed" from it.
        sys_eprint("wm: no desktop entries in " DESKTOP_DIR " -- Start menu is empty\n");
    } else {
        wm_logf("wm: %d desktop entries from " DESKTOP_DIR "\n",
                     gui_app_registry_count);
    }
}

// --- per-surface views of the registry (see gui_apps.h) ---------------
//
// A linear scan per call, which is the right trade at this size: the
// registry holds a dozen entries, the callers are a menu draw and a hit
// test, and the alternative -- a cached filtered index array -- is a
// second copy of the registry that can go stale when gui_apps_load()
// re-runs. Correct by construction beats fast at eleven items.

int gui_app_shows_in(const struct gui_app *app, unsigned surface) {
    return app && (app->show_in & surface) != 0;
}

int gui_app_visible_count(unsigned surface) {
    int n = 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (gui_app_shows_in(&gui_app_registry[i], surface)) n++;
    }
    return n;
}

struct gui_app *gui_app_visible_at(unsigned surface, int n) {
    if (n < 0) return 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (!gui_app_shows_in(&gui_app_registry[i], surface)) continue;
        if (n-- == 0) return &gui_app_registry[i];
    }
    return 0; // out of range: a no-op, never a clamp onto the last entry
}
