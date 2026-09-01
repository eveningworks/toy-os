// See driver.h.
#include "driver.h"
#include "query.h"
#include "string.h"
#include "klog.h"
#include <stddef.h>

struct entry {
    char name[DRIVER_NAME_MAX];
    char cls[DRIVER_CLASS_MAX];
    char file[DRIVER_FILE_MAX];
    char devs[DRIVER_DEVS_MAX];
};

static struct entry g_drv[DRIVER_MAX];
static int g_count;

static struct entry *find(const char *name) {
    for (int i = 0; i < g_count; i++)
        if (k_strcmp(g_drv[i].name, name) == 0) return &g_drv[i];
    return NULL;
}

static struct entry *add(const char *name, const char *cls, const char *file) {
    if (g_count >= DRIVER_MAX) {
        // Loudly, once per overflow: a driver missing from the listing
        // looks like a driver that is not in the build, which is the
        // wrong conclusion to hand somebody debugging hardware.
        klog_write("driver: registry full -- a driver will not be listed\n");
        return NULL;
    }
    struct entry *e = &g_drv[g_count++];
    k_strlcpy(e->name, name, sizeof e->name);
    k_strlcpy(e->cls, cls ? cls : "?", sizeof e->cls);
    k_strlcpy(e->file, file ? file : "", sizeof e->file);
    e->devs[0] = '\0';
    return e;
}

void driver_register_at(const char *name, const char *cls, const char *file) {
    if (!name || !name[0]) return;
    struct entry *e = find(name);
    if (e) {
        // A second registration fills in what the first did not know --
        // which is how a driver that was created by driver_bound()
        // (class "?") gets its real class when its init runs later.
        if (cls && k_strcmp(e->cls, "?") == 0) k_strlcpy(e->cls, cls, sizeof e->cls);
        if (file && !e->file[0]) k_strlcpy(e->file, file, sizeof e->file);
        return;
    }
    add(name, cls, file);
}

void driver_bound(const char *name, const char *dev) {
    if (!name || !name[0] || !dev || !dev[0]) return;
    struct entry *e = find(name);
    if (!e) e = add(name, "?", "");
    if (!e) return;

    // Space-separated, and SILENTLY CAPPED rather than truncated
    // mid-name: half a device name in a listing reads as a device that
    // does not exist. A driver with more devices than fit is rare
    // enough that the cap is better than a per-driver array.
    uint32_t have = (uint32_t)k_strlen(e->devs);
    uint32_t want = (uint32_t)k_strlen(dev);
    uint32_t need = have ? have + 1 + want : want;
    if (need + 1 > DRIVER_DEVS_MAX) return;
    if (have) e->devs[have++] = ' ';
    k_strlcpy(e->devs + have, dev, DRIVER_DEVS_MAX - have);
}

int driver_count(void) { return g_count; }
const char *driver_name_at(int i)  { return (i >= 0 && i < g_count) ? g_drv[i].name : ""; }
const char *driver_class_at(int i) { return (i >= 0 && i < g_count) ? g_drv[i].cls  : ""; }
const char *driver_file_at(int i)  { return (i >= 0 && i < g_count) ? g_drv[i].file : ""; }
const char *driver_devices_at(int i) { return (i >= 0 && i < g_count) ? g_drv[i].devs : ""; }

// --- the provider ------------------------------------------------------

static int drv_count(void) { return g_count; }

static int drv_fill(int index, void *out) {
    if (index < 0 || index >= g_count) return 0;
    struct query_driver *q = out;
    k_memset(q, 0, sizeof *q);
    k_strlcpy(q->name,    g_drv[index].name, sizeof q->name);
    k_strlcpy(q->cls,     g_drv[index].cls,  sizeof q->cls);
    k_strlcpy(q->file,    g_drv[index].file, sizeof q->file);
    k_strlcpy(q->devices, g_drv[index].devs, sizeof q->devices);
    return 1;
}

static const struct query_provider drv_provider = {
    .cls = QUERY_DRIVER,
    .name = "driver",
    .record_size = sizeof(struct query_driver),
    .flags = QUERY_F_LIST,
    .count = drv_count,
    .fill = drv_fill,
    .fields = NULL,
    .field_count = 0,
};

void driver_query_init(void) { query_register(&drv_provider); }
