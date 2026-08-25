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
// SIZED FROM THE REAL WORKING SET, which is apps x sizes: ~14 registry
// entries against the desktop grid, the taskbar, a title bar, a Start
// menu row, the Start button and a sidebar. 32 was set when the comment
// below said "eleven apps and three sizes" -- which is 33, already over
// its own cap, and the wholesale drop below then re-decoded every icon
// on EVERY frame. Measured: opening the Start menu took a desktop from
// 30 cached to the cap, after which the count collapsed to 18, 20, 14,
// 6 frame after frame and the WM logged 100-260 ms frames.
#define ICON_CACHE_MAX 96

struct icon_entry {
    char name[ICON_NAME_MAX];
    int size;
    int missing;          // looked for and not there: remembered, so a
                          // fallback icon costs one failed open, not one
                          // per frame
    unsigned long long used; // last hit, for the LRU below
    struct uimg img;      // scaled to `size`; px is NULL when missing
};

static struct icon_entry g_cache[ICON_CACHE_MAX];
static int g_count;
static unsigned long long g_clock;   // monotonic, for `used`
static int g_evictions;              // reported by `gui icons`

void icon_cache_invalidate(void) {
    for (int i = 0; i < g_count; i++) uimg_free(&g_cache[i].img);
    k_memset(g_cache, 0, sizeof g_cache);
    g_count = 0;
}

int icon_cache_count(void) { return g_count; }
int icon_cache_evictions(void) { return g_evictions; }

static struct icon_entry *find(const char *name, int size) {
    for (int i = 0; i < g_count; i++)
        if (g_cache[i].size == size && k_strcmp(g_cache[i].name, name) == 0)
            return &g_cache[i];
    return NULL;
}

const struct uimg *icon_get(const char *name, int size) {
    if (!name || !name[0] || size <= 0) return NULL;

    struct icon_entry *e = find(name, size);
    if (e) { e->used = ++g_clock; return e->missing ? NULL : &e->img; }

    // FULL DROPS ONE ENTRY, NOT ALL OF THEM. It used to drop the cache
    // wholesale, on the reasoning that the cap was never reached -- and
    // when it was, the next frame re-decoded everything and hit the cap
    // again, so the cache became a decode-per-frame with extra steps.
    // A single LRU eviction degrades instead: a working set one entry
    // over the cap costs one decode per frame, not thirty.
    //
    // THE CAP IS STILL WHAT MATTERS. An LRU over a cap below the working
    // set thrashes just as surely, only more slowly -- which is why
    // `gui icons` reports the eviction count, so the next time this is
    // too small it says so instead of being silently slow.
    if (g_count >= ICON_CACHE_MAX) {
        int lru = 0;
        for (int i = 1; i < g_count; i++)
            if (g_cache[i].used < g_cache[lru].used) lru = i;
        if (!g_evictions)
            ulogf("icons: cache full (%d entries) -- evicting, see `gui icons`\n",
                  ICON_CACHE_MAX);
        g_evictions++;
        uimg_free(&g_cache[lru].img);
        g_cache[lru] = g_cache[g_count - 1];
        g_count--;
    }

    e = &g_cache[g_count++];
    e->used = ++g_clock;
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
