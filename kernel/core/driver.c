// See driver.h.
#include "driver.h"
#include "query.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h" // klog_printf
#include <stddef.h>
#include "initcall.h"
#include "errno.h"

// The declarations the linker collected. Walked in place rather than
// copied: they are `const char *` into .rodata, which the relocator has
// already fixed up by the time anything asks.
extern const struct driver_decl __drivers_start[];
extern const struct driver_decl __drivers_end[];

// A driver named by driver_bound() that declared nothing. It should not
// happen -- tools/check_drivers.py fails the build on a driver file with
// no DRIVER_DECLARE -- but a binding nobody can see is the thing this
// module exists to fix, so it is recorded rather than dropped.
struct extra {
    char name[DRIVER_NAME_MAX];
    char devs[DRIVER_DEVS_MAX];
};

// Bound devices, one slot per DECLARATION and indexed by its position in
// the section. Kept beside the declarations rather than in them because
// a declaration is const data in the image and a binding is discovered
// at boot.
static char g_devs[DRIVER_MAX][DRIVER_DEVS_MAX];
static struct extra g_extra[8];
static int g_extra_count;

// Declarations that are NOT in the image: a loaded module's `.drivers`
// table. Indexed after the image's declarations and before the extras,
// each with its own bound-device strings, and a slot is reused rather
// than shifted so the strings stay with their table.
#define DRIVER_TABLES_MAX     8
#define DRIVER_TABLE_DECLS_MAX 4
struct table {
    const struct driver_decl *decls;   // NULL when the slot is free
    int n;
    char devs[DRIVER_TABLE_DECLS_MAX][DRIVER_DEVS_MAX];
};
static struct table g_tables[DRIVER_TABLES_MAX];

// One entry of the whole index space: a declaration and its device
// string, or an extra (decl NULL).
struct slot {
    const struct driver_decl *decl;
    char *devs;
    const struct extra *extra;
};

static int decl_count(void) {
    long n = __drivers_end - __drivers_start;
    if (n < 0) n = 0;
    if (n > DRIVER_MAX) n = DRIVER_MAX;   // g_devs is what bounds this
    return (int)n;
}

// Removes `dev` from a space-separated list, closing the gap. Matching
// is on a WHOLE entry: "net1" must not match inside "net12", which is
// reachable now that names can be several characters long.
static void remove_dev(char *devs, const char *dev) {
    uint32_t want = (uint32_t)k_strlen(dev);
    if (!want) return;
    for (uint32_t i = 0; devs[i]; ) {
        uint32_t end = i;
        while (devs[end] && devs[end] != ' ') end++;
        if (end - i == want && k_memcmp(devs + i, dev, want) == 0) {
            // Take the separator with it: the one BEFORE when this is
            // the last entry, the one after otherwise, so the list
            // never gains a leading or doubled space.
            uint32_t from = devs[end] == ' ' ? end + 1 : end;
            uint32_t to = (devs[end] != ' ' && i > 0) ? i - 1 : i;
            k_memmove(devs + to, devs + from, k_strlen(devs + from) + 1);
            return;
        }
        i = devs[end] ? end + 1 : end;
    }
}

static int table_decls(void) {
    int n = 0;
    for (int t = 0; t < DRIVER_TABLES_MAX; t++)
        if (g_tables[t].decls) n += g_tables[t].n;
    return n;
}

// Resolves index `i` across the three halves: the image, the module
// tables, the extras. Returns 0 past the end.
static int slot_at(int i, struct slot *out) {
    if (i < 0) return 0;
    if (i < decl_count()) {
        out->decl = &__drivers_start[i]; out->devs = g_devs[i]; out->extra = 0;
        return 1;
    }
    i -= decl_count();
    for (int t = 0; t < DRIVER_TABLES_MAX; t++) {
        if (!g_tables[t].decls) continue;
        if (i < g_tables[t].n) {
            out->decl = &g_tables[t].decls[i]; out->devs = g_tables[t].devs[i];
            out->extra = 0;
            return 1;
        }
        i -= g_tables[t].n;
    }
    if (i < g_extra_count) {
        out->decl = 0; out->devs = g_extra[i].devs; out->extra = &g_extra[i];
        return 1;
    }
    return 0;
}

// The device string of the DECLARED driver `name`, or NULL.
static char *devs_of(const char *name) {
    struct slot sl;
    for (int i = 0; slot_at(i, &sl); i++) {
        if (sl.decl) {
            if (sl.decl->name && k_strcmp(sl.decl->name, name) == 0) return sl.devs;
        } else if (k_strcmp(sl.extra->name, name) == 0) {
            return sl.devs;
        }
    }
    return 0;
}

// Appends `dev` to a space-separated list, SILENTLY CAPPED rather than
// truncated mid-name: half a device name in a listing reads as a device
// that does not exist. A driver with more devices than fit is rare
// enough that the cap is better than a per-driver array.
static void append_dev(char *devs, const char *dev) {
    uint32_t have = (uint32_t)k_strlen(devs);
    uint32_t want = (uint32_t)k_strlen(dev);
    uint32_t need = have ? have + 1 + want : want;
    if (need + 1 > DRIVER_DEVS_MAX) return;
    if (have) devs[have++] = ' ';
    k_strlcpy(devs + have, dev, DRIVER_DEVS_MAX - have);
}

// The inverse, for a device that has been unplugged. An unknown driver
// or device is ignored, so a class registry may call it unconditionally.
void driver_unbound(const char *name, const char *dev) {
    if (!name || !name[0] || !dev || !dev[0]) return;
    char *devs = devs_of(name);
    if (devs) remove_dev(devs, dev);
}

void driver_bound(const char *name, const char *dev) {
    if (!name || !name[0] || !dev || !dev[0]) return;

    char *devs = devs_of(name);
    if (devs) { append_dev(devs, dev); return; }

    if (g_extra_count >= (int)(sizeof g_extra / sizeof g_extra[0])) {
        klog_write("driver: no room to record an undeclared driver\n");
        return;
    }
    klog_printf("driver: %s bound %s but declares itself nowhere "
                "-- add DRIVER_DECLARE\n", name, dev);
    struct extra *e = &g_extra[g_extra_count++];
    k_strlcpy(e->name, name, sizeof e->name);
    e->devs[0] = '\0';
    append_dev(e->devs, dev);
}

int driver_add_table(const struct driver_decl *decls, int n) {
    if (!decls || n <= 0 || n > DRIVER_TABLE_DECLS_MAX) return -EINVAL;
    int free_slot = -1;
    for (int t = 0; t < DRIVER_TABLES_MAX; t++) {
        if (g_tables[t].decls == decls) return -EEXIST;
        if (!g_tables[t].decls && free_slot < 0) free_slot = t;
    }
    if (free_slot < 0) return -ENOSPC;
    g_tables[free_slot].decls = decls;
    g_tables[free_slot].n = n;
    k_memset(g_tables[free_slot].devs, 0, sizeof g_tables[free_slot].devs);
    return 0;
}

int driver_remove_table(const struct driver_decl *decls) {
    for (int t = 0; t < DRIVER_TABLES_MAX; t++) {
        if (g_tables[t].decls != decls) continue;
        g_tables[t].decls = 0;
        g_tables[t].n = 0;
        return 0;
    }
    return -ENOENT;
}

int driver_count(void) { return decl_count() + table_decls() + g_extra_count; }

const char *driver_name_at(int i) {
    struct slot sl;
    if (!slot_at(i, &sl)) return "";
    if (sl.decl) return sl.decl->name ? sl.decl->name : "";
    return sl.extra->name;
}

const char *driver_class_at(int i) {
    struct slot sl;
    if (!slot_at(i, &sl)) return "";
    if (sl.decl) return sl.decl->cls ? sl.decl->cls : "";
    return "?";
}

const char *driver_file_at(int i) {
    struct slot sl;
    if (!slot_at(i, &sl) || !sl.decl) return "";
    return sl.decl->file ? sl.decl->file : "";
}

const char *driver_desc_at(int i) {
    struct slot sl;
    if (!slot_at(i, &sl) || !sl.decl) return "";
    return sl.decl->desc ? sl.decl->desc : "";
}

const char *driver_devices_at(int i) {
    struct slot sl;
    return slot_at(i, &sl) ? sl.devs : "";
}

// --- the provider ------------------------------------------------------

static int drv_count(void) { return driver_count(); }

static int drv_fill(int index, void *out) {
    if (index < 0 || index >= driver_count()) return 0;
    struct query_driver *q = out;
    k_memset(q, 0, sizeof *q);
    k_strlcpy(q->name,    driver_name_at(index),    sizeof q->name);
    k_strlcpy(q->cls,     driver_class_at(index),   sizeof q->cls);
    k_strlcpy(q->file,    driver_file_at(index),    sizeof q->file);
    k_strlcpy(q->desc,    driver_desc_at(index),    sizeof q->desc);
    k_strlcpy(q->devices, driver_devices_at(index), sizeof q->devices);
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
INITCALL(driver_query_init, INIT_QUERY);
