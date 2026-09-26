// The config-file registry -- see config_file.h for what it indexes and
// why it has two sources.
//
// The descriptor scan is deliberately the same shape as apps/gui_apps.c's
// `.desktop` scan: list a directory, read a few keys out of each file
// through etc_config_get(), register what parses, ignore what does not.
// A malformed descriptor is skipped rather than failing the scan -- one
// bad file must not cost the machine its whole configuration index.
#include "config_file.h"
#include "etc_config.h"
#include "string.h"
#include "fs.h"
#include "kpath.h"
#include "heap.h"   // kmalloc -- the scan's state is the call's

static struct config_file g_files[CONFIG_FILE_MAX];
static int g_count = 0;

int config_file_count(void) { return g_count; }

const struct config_file *config_file_at(int index) {
    if (index < 0 || index >= g_count) return 0;
    return &g_files[index];
}

const struct config_file *config_file_find_by_path(const char *path) {
    if (!path || !*path) return 0;
    for (int i = 0; i < g_count; i++) {
        if (k_strcmp(g_files[i].path, path) == 0) return &g_files[i];
    }
    return 0;
}

const struct config_file *config_file_find(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_count; i++) {
        if (k_strcmp(g_files[i].name, name) == 0) return &g_files[i];
    }
    return 0;
}

int config_file_register(const char *name, const char *path, const char *desc, int builtin) {
    if (!name || !*name || !path || !*path) return 0;
    if (k_strlen(name) >= CONFIG_NAME_MAX) return 0;
    if (k_strlen(path) >= CONFIG_PATH_MAX) return 0;
    if (desc && k_strlen(desc) >= CONFIG_DESC_MAX) return 0;

    struct config_file *slot = 0;
    for (int i = 0; i < g_count; i++) {
        if (k_strcmp(g_files[i].name, name) != 0) continue;
        // The override rule (config_file.h): a descriptor may replace a
        // built-in. Anything else colliding is refused, because which
        // one won would depend on directory or boot order.
        if (builtin || !g_files[i].builtin) return 0;
        slot = &g_files[i];
        break;
    }
    if (!slot) {
        if (g_count >= CONFIG_FILE_MAX) return 0;
        slot = &g_files[g_count++];
    }

    k_strlcpy(slot->name, name, sizeof slot->name);
    k_strlcpy(slot->path, path, sizeof slot->path);
    k_strlcpy(slot->desc, desc ? desc : "", sizeof slot->desc);
    slot->builtin = builtin;
    return 1;
}

// --- the descriptor scan ---------------------------------------------

// The walk collects names first and reads them afterwards -- reading a
// file from inside an fs_list() callback would re-enter the filesystem
// mid-walk. The names and the read buffer are the CALL's (fs.h's
// fs_list() on why nothing a walk touches may be a global), heap-held
// because together they are too big for a kernel frame.
#define SCAN_MAX 16
struct scan_ctx {
    char names[SCAN_MAX][CONFIG_NAME_MAX + 8];
    int count;
    struct etc_config_buf buf;   // one read per descriptor, three keys out of it
};

static void scan_collect(void *ctx, const char *name, uint32_t size, int is_dir) {
    struct scan_ctx *x = ctx;
    (void)size;
    if (is_dir || x->count >= SCAN_MAX) return;
    if (k_strlen(name) >= (int)sizeof x->names[0]) return;
    k_strlcpy(x->names[x->count++], name, sizeof x->names[0]);
}

void config_files_scan(void) {
    // Drop everything that came from disk and rebuild it, so a
    // descriptor DELETED from /etc/config.d actually disappears. A scan
    // that only ever added would make `config unregister` look like it
    // had done nothing until the next reboot.
    int keep = 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_files[i].builtin) continue;
        if (keep != i) g_files[keep] = g_files[i];
        keep++;
    }
    g_count = keep;

    // The kernel's own. Registered here rather than by each owning
    // subsystem because three of the four are DATA files with no
    // subsystem that would naturally announce them -- and a rescan has
    // to be able to put the built-in floor back after an override was
    // withdrawn.
    config_file_register("system", "/etc/toyos.conf",
                         "System settings", 1);
    config_file_register("timezones", "/etc/timezones",
                         "The timezone city database", 1);
    config_file_register("desktop", "/etc/desktop.conf",
                         "Desktop icon positions and wallpaper", 1);
    config_file_register("storage", "/etc/storage.conf",
                         "Filesystem and disk behaviour", 1);
    config_file_register("shortcuts", "/etc/shortcuts.conf",
                         "Global keyboard shortcuts", 1);
    config_file_register("windows", "/etc/windows.conf",
                         "Remembered window positions and sizes", 1);
    config_file_register("sound", "/etc/sound.conf",
                         "Per-application playback volume", 1);
    config_file_register("keymaps", "/etc/kbs",
                         "Keyboard layout tables (dir)", 1);
    config_file_register("apps", "/usr/wm/applications",
                         "Start-menu/desktop entries (dir)", 1);
    // The one namespace with NO file. Registered like the others so
    // that `kernel.heap_debug` resolves through the same path lookup
    // every other setting's namespace does -- see CONFIG_PATH_RUNTIME.
    config_file_register(CONFIG_NAME_RUNTIME, CONFIG_PATH_RUNTIME,
                         "Kernel tunables -- runtime only, reset at boot", 1);

    struct scan_ctx *x = kmalloc(sizeof *x);
    if (!x) return;   // the built-in floor above still stands
    x->count = 0;
    fs_list(CONFIG_DESCRIPTOR_DIR, scan_collect, x);

    for (int i = 0; i < x->count; i++) {
        char full[CONFIG_PATH_MAX + CONFIG_NAME_MAX + 8];
        char name[CONFIG_NAME_MAX], path[CONFIG_PATH_MAX], desc[CONFIG_DESC_MAX];

        if (!k_path_join(CONFIG_DESCRIPTOR_DIR, x->names[i], full, sizeof full)) continue;

        // Name and Path are required; Description is not. A descriptor
        // missing either is skipped silently -- it is a file someone
        // may still be writing, and one bad file must not cost the
        // whole index.
        if (!etc_config_load(full, &x->buf)) continue;   // unreadable or oversize: skipped
        if (!etc_config_buf_get(&x->buf, "Name", name, sizeof name)) continue;
        if (!etc_config_buf_get(&x->buf, "Path", path, sizeof path)) continue;
        if (!etc_config_buf_get(&x->buf, "Description", desc, sizeof desc)) desc[0] = '\0';
        if (!name[0] || path[0] != '/') continue;

        config_file_register(name, path, desc, 0);
    }
    kfree(x);
}

int config_file_is_runtime(const struct config_file *f) {
    return f && k_strcmp(f->path, CONFIG_PATH_RUNTIME) == 0;
}
