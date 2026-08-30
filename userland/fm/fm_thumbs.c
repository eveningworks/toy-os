// Thumbnails for the icons view -- a LOOKUP on the draw path, a
// decode on the tick.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "kpath.h"
#include <string.h>
#include <stdio.h>   // fopen/fread -- the decoder is handed bytes

// --- thumbnails ---------------------------------------------------------
//
// The icons view asks uui_fileview's thumb callback per visible cell,
// and that callback must be a LOOKUP (uui_fileview.h says why). The
// decode happens HERE, on the tick, at most a couple per pass -- the
// lazy shape every desktop thumbnailer has, minus the daemon: a folder
// of photos populates over a few ticks instead of freezing the window
// for as many full JPEG decodes as it has files.
//
// Keyed by PATH + MTIME + SIZE-ON-DISK, so a rewritten file re-decodes
// and a renamed one simply misses. LRU over a fixed table, icon_cache's
// arrangement; ~4 KB of pixels per ready entry at the default font.
#define THUMB_MAX 48
#define THUMB_PER_TICK 2

enum thumb_state { THUMB_EMPTY = 0, THUMB_PENDING, THUMB_READY, THUMB_NOT_IMAGE };

struct thumb {
    char path[PATH_MAX_LEN];
    struct rtc_time mtime;
    uint32_t fsize;
    unsigned long long used;  // LRU clock; also "most recently WANTED"
    int px;
    enum thumb_state state;
    struct uimg im;           // valid when READY
};

struct thumb g_thumbs[THUMB_MAX];
static unsigned long long g_thumb_clock;

static struct thumb *thumb_slot(const char *path) {
    struct thumb *lru = &g_thumbs[0];
    for (int i = 0; i < THUMB_MAX; i++) {
        if (g_thumbs[i].state != THUMB_EMPTY &&
            strcmp(g_thumbs[i].path, path) == 0)
            return &g_thumbs[i];
        if (g_thumbs[i].used < lru->used) lru = &g_thumbs[i];
    }
    uimg_free(&lru->im); // safe on a never-decoded image (uimg.h)
    memset(lru, 0, sizeof *lru);
    strlcpy(lru->path, path, sizeof lru->path);
    return lru;
}

// The fileview's callback: a lookup that may ENQUEUE, never a decode.
const struct uimg *pane_thumb(void *ctx, const char *dir,
                                      const struct sys_dirent *e, int px) {
    (void)ctx;
    char path[PATH_MAX_LEN];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;

    struct thumb *t = thumb_slot(path);
    t->used = ++g_thumb_clock;

    int stale = t->state == THUMB_EMPTY || t->px != px ||
                 t->fsize != e->size ||
                 memcmp(&t->mtime, &e->modified, sizeof t->mtime) != 0;
    if (stale) {
        t->mtime = e->modified;
        t->fsize = e->size;
        t->px = px;
        uimg_free(&t->im);
        t->state = THUMB_PENDING;
    }
    return t->state == THUMB_READY ? &t->im : 0;
}

// Decode the most recently WANTED pending entries -- "wanted" is the
// LRU clock the lookup stamps, so what is on screen populates first.
// Returns 1 if anything became ready (the caller repaints).
int thumb_tick(void) {
    int changed = 0;
    for (int n = 0; n < THUMB_PER_TICK; n++) {
        struct thumb *pick = 0;
        for (int i = 0; i < THUMB_MAX; i++)
            if (g_thumbs[i].state == THUMB_PENDING &&
                (!pick || g_thumbs[i].used > pick->used))
                pick = &g_thumbs[i];
        if (!pick) break;

        // Sniff before loading: uimg_load() reads the WHOLE file, and
        // most files in a directory are not images.
        uint8_t head[16];
        FILE *f = fopen(pick->path, "rb");
        size_t got = f ? fread(head, 1, sizeof head, f) : 0;
        if (f) fclose(f);
        if (got < 4 || !uimg_probe(head, got)) {
            pick->state = THUMB_NOT_IMAGE;
            continue;
        }

        struct uimg full;
        if (uimg_load(pick->path, &full) != 0) {
            pick->state = THUMB_NOT_IMAGE; // broken or refused: the icon
            continue;
        }
        int tw, th;
        uimg_fit_size(full.w, full.h, pick->px, pick->px, UIMG_FIT_CONTAIN,
                       &tw, &th);
        int rc = uimg_scale(&full, tw, th, &pick->im);
        uimg_free(&full);
        pick->state = rc == 0 ? THUMB_READY : THUMB_NOT_IMAGE;
        if (rc == 0) changed = 1;
    }
    return changed;
}
