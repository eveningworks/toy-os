// See icon_cache.h.
#include "lib/icon_cache.h"
// ulog(), not the WM's logger: this moved out of userland/wm/ when the
// toolkit's sidebar needed icons too, and a library that logs through
// one app's helper is a library only that app can link.
#include "ui/ulog.h"
#include "string.h"
#include "kfmt.h"

#define ICON_DIR       "/usr/share/icons"
#define ICON_NAME_MAX  24
#define ICON_CACHE_MAX 32

struct icon_entry {
    char name[ICON_NAME_MAX];
    int size;
    int missing;          // looked for and not there: remembered, so a
                          // fallback icon costs one failed open, not one
                          // per frame
    struct uimg img;      // scaled to `size`; px is NULL when missing
};

static struct icon_entry g_cache[ICON_CACHE_MAX];
static int g_count;

void icon_cache_invalidate(void) {
    for (int i = 0; i < g_count; i++) uimg_free(&g_cache[i].img);
    k_memset(g_cache, 0, sizeof g_cache);
    g_count = 0;
}

int icon_cache_count(void) { return g_count; }

static struct icon_entry *find(const char *name, int size) {
    for (int i = 0; i < g_count; i++)
        if (g_cache[i].size == size && k_strcmp(g_cache[i].name, name) == 0)
            return &g_cache[i];
    return NULL;
}

const struct uimg *icon_get(const char *name, int size) {
    if (!name || !name[0] || size <= 0) return NULL;

    struct icon_entry *e = find(name, size);
    if (e) return e->missing ? NULL : &e->img;

    // FULL IS FULL: the cache is dropped wholesale rather than evicting
    // one entry. There are eleven apps and three sizes on this desktop,
    // so the cap is not reached in practice -- and an LRU would be a
    // policy with no measurement behind it. If this ever starts
    // thrashing, that is the moment to measure and pick one.
    if (g_count >= ICON_CACHE_MAX) icon_cache_invalidate();

    e = &g_cache[g_count++];
    k_strlcpy(e->name, name, sizeof e->name);
    e->size = size;
    e->missing = 1;

    char path[64];
    k_snprintf(path, sizeof path, "%s/%s.qoi", ICON_DIR, name);

    struct uimg master;
    if (uimg_load(path, &master) < 0) {
        // Logged ONCE per (name, size), because the entry is kept as a
        // negative result -- an icon that is missing must not produce a
        // line per repaint.
        ulogf("icons: %s not loaded -- %s\n", path, uimg_last_error());
        return NULL;
    }

    int rc = uimg_scale(&master, size, size, &e->img);
    uimg_free(&master);
    if (rc < 0) {
        ulogf("icons: %s could not be scaled to %d -- %s\n", path, size,
                uimg_last_error());
        return NULL;
    }
    e->missing = 0;
    return &e->img;
}
