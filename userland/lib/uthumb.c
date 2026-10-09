// uthumb -- the thumbnail cache. See uthumb.h for the shape; the File
// Manager's icons view (fm_thumbs.c) and the Image Viewer's filmstrip
// draw through it.
#include "lib/uthumb.h"
#include "lib/ufile.h"
#include "lib/uvid.h"
#include "lib/uduration.h"
#include "kpath.h"
#include "caltime.h"   // cal_rtc_to_epoch -- the KERNEL's, linked into ring 3
#include "ui/ulog.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#define THUMB_MAX 96
#define THUMB_PATH 128
// The disk cache. Both real desktops keep one -- ~/.cache/thumbnails
// under the freedesktop Thumbnail Managing Standard, thumbcache_*.db on
// Windows -- because the second visit to a folder of photographs should
// not pay for the first one again.
#define THUMB_CACHE_DIR  "/var/cache/thumbnails"
#define THUMB_CACHE_MAX  512      // files kept there; the oldest go first
#define THUMB_SWEEP_EVERY 128     // writes between sweeps

enum thumb_state { THUMB_EMPTY = 0, THUMB_PENDING, THUMB_DECODING,
                   THUMB_READY, THUMB_NOT_IMAGE };

struct thumb {
    char path[THUMB_PATH];
    struct rtc_time mtime;
    uint32_t fsize;
    unsigned long long used;  // LRU clock; also "most recently WANTED"
    int px;
    enum thumb_state state;
    struct uimg im;           // valid when READY
    char note[12];            // a video's length, "1:17"; "" for none
};

static struct uthumb_config g_cfg;
static struct thumb g_thumbs[THUMB_MAX];
static unsigned long long g_thumb_clock;

// THE HANDOVER, and the only thing the worker touches. The main thread
// fills it in, publishes it, and does not look again until the post;
// the worker writes only into it. One decode at a time is what makes
// that enough -- there is no lock here and nothing to lock.
struct thumb_job {
    char path[THUMB_PATH];
    char cache[THUMB_PATH];        // empty when this file gets no cache file
    uint64_t src_epoch;            // the SOURCE's mtime, for the freshness test
    int px;
    struct uimg im;
    int rc;                        // 0 = a picture, -1 = not one
    int cached;                    // it came off the disk, not a decoder
    char note[12];                 // a video's length
};

static struct thumb_job *g_job;    // in flight
static int g_kick_posted;          // a wake-up is already queued

// What one DRAIN cost, reported when the queue empties: the only outside
// evidence of the rate (tools/thumbcache_test.py reads it).
static unsigned g_batch, g_batch_cached;
static unsigned long long g_batch_t0;

static int wake(void) { return g_cfg.wake ? g_cfg.wake(g_cfg.ctx) : 0; }

void uthumb_init(const struct uthumb_config *cfg) { g_cfg = *cfg; }

// --- the disk cache -----------------------------------------------------

// The cache file for one (source path, size in pixels). THE NAME IS THE
// SOURCE PATH with '/' written '%': the directory then says what it
// holds, `ls` is the debugger, and two sources cannot collide -- where a
// hash would need a second file recording what each name stood for,
// which is exactly why the freedesktop spec has to store Thumb::URI
// inside its PNGs. QOI has nowhere to put one.
//
// Refused when the result will not fit sys_dirent's 64-byte name, which
// cache_sweep() lists through, and for a source with a '%' in it, which
// would read back as a '/' (`/a%b` and `/a/b` would share a file);
// either is thumbnailed in memory only.
static int cache_path(const char *src, int px, char *out, int cap) {
    char m[64];
    size_t n = 0;
    for (const char *p = src; *p; p++) {
        if (*p == '%') return 0;
        if (*p == '/' && n == 0) continue;   // no leading '%'
        if (n + 1 >= sizeof m) return 0;
        m[n++] = (*p == '/') ? '%' : *p;
    }
    m[n] = '\0';
    if (!n) return 0;

    char name[64];
    if (snprintf(name, sizeof name, "%s-%d.qoi", m, px) >= (int)sizeof name)
        return 0;
    int len = snprintf(out, (size_t)cap, THUMB_CACHE_DIR "/%s", name);
    return len > 0 && len < cap;
}

// A cache entry is good while it is NEWER than its source -- make's
// rule, and the one staleness test that needs no metadata in a format
// with none to give. STRICTLY newer: mtimes are whole seconds, so an
// entry written in the same second as a rewrite would tie and be served
// stale ("racy git"); a tie costs one extra decode. A rewritten source
// is newer, so the next look
// re-decodes and OVERWRITES this same name: the directory is bounded by
// the number of distinct (file, size) pairs, not by how often they
// change.
static int cache_fresh(const char *cache, uint64_t src_epoch) {
    struct sys_stat st;
    if (sys_stat(cache, &st) != 0) return 0;
    return cal_rtc_to_epoch(&st.modified) > src_epoch;
}

// The oldest files go first. With the rule above a HIT does not rewrite
// its file, so this evicts by age rather than by use -- an
// approximation, and the cheap one: recording a use would mean writing
// to the disk on every cache hit.
//
// PAGED: one sys_listdir() returns at most SYS_LISTDIR_MAX names, fewer
// than THUMB_CACHE_MAX, so a single listing could never see enough files
// to evict any. A pass counts them and keeps the THUMB_SWEEP_EVERY
// oldest; passes repeat while the excess is larger (a cache that grew
// before this bound held), and stop when a pass removes nothing.
struct sweep_victim { uint64_t epoch; char name[sizeof ((struct sys_dirent *)0)->name]; };

static int sweep_pass(struct sys_dirent *e, struct sweep_victim *old) {
    int files = 0, kept = 0, newest = 0;   // `newest`: the kept one to replace next
    for (int start = 0;; ) {
        int n = sys_listdir_at(THUMB_CACHE_DIR, e, SYS_LISTDIR_MAX, start);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (e[i].is_dir) continue;
            files++;
            uint64_t t = cal_rtc_to_epoch(&e[i].modified);
            if (kept == THUMB_SWEEP_EVERY && t >= old[newest].epoch) continue;
            int slot = kept < THUMB_SWEEP_EVERY ? kept++ : newest;
            old[slot].epoch = t;
            strlcpy(old[slot].name, e[i].name, sizeof old[slot].name);
            for (int k = 0; k < kept; k++)
                if (old[k].epoch > old[newest].epoch) newest = k;
        }
        start += n;
        if (n < SYS_LISTDIR_MAX) break;
    }
    // Oldest first, so an excess smaller than `kept` spares the newest.
    int removed = 0;
    for (int drop = files - THUMB_CACHE_MAX; drop > 0 && kept > 0; drop--) {
        int o = 0;
        for (int k = 1; k < kept; k++)
            if (old[k].epoch < old[o].epoch) o = k;
        char p[THUMB_PATH];
        if (k_path_join(THUMB_CACHE_DIR, old[o].name, p, sizeof p) && sys_unlink(p) >= 0)
            removed++;
        old[o] = old[--kept];
    }
    return files - removed > THUMB_CACHE_MAX && removed > 0;   // go again
}

static void cache_sweep(void) {
    struct sys_dirent *e = malloc(sizeof *e * SYS_LISTDIR_MAX);
    struct sweep_victim *old = malloc(sizeof *old * THUMB_SWEEP_EVERY);
    if (e && old)
        while (sweep_pass(e, old)) {}
    free(old);
    free(e);
}

// Best effort: a cache that cannot be written is a slower app, never a
// broken one, so nothing here reports a failure.
//
// **THE MKDIR IS A RETRY, NOT A STARTUP STEP.** Deleting this directory
// is allowed -- it is a cache (docs/filesystem-layout.md) -- so doing it
// once meant an `rm -r` stopped the caching silently for the life of the
// process, with thumbnails still appearing and nothing to say they were
// no longer being kept.
static void cache_store(const char *cache, const struct uimg *im) {
    static unsigned writes;
    if (writes++ % THUMB_SWEEP_EVERY == 0) cache_sweep();
    if (uimg_save(cache, im, "qoi") == 0) return;
    sys_mkdir("/var/cache");
    sys_mkdir(THUMB_CACHE_DIR);
    uimg_save(cache, im, "qoi");
}

// --- the worker ---------------------------------------------------------

// Everything below runs off the main thread and touches nothing but its
// own job.
static int thumb_produce(struct thumb_job *j) {
    // A VIDEO carries a NOTE beside its picture, its length -- read
    // from its index here, off the draw path, cached copy or not (the
    // cache holds pictures only).
    uint8_t head[16];
    size_t got = ufile_read_head(j->path, head, sizeof head);
    int video = got >= 4 && uvid_probe(head, got);
    struct uvid_info in;
    if (video && uvid_load_info(j->path, &in) == 0 && in.ms)
        uduration_clock(in.ms, j->note, sizeof j->note);
    else
        video = video ? 2 : 0;          // a video whose index will not read

    // THE CACHE FIRST: one small QOI read instead of a full decode and a
    // rescale, which for a photograph is the difference the cache exists
    // for. A miss costs one stat.
    if (j->cache[0] && cache_fresh(j->cache, j->src_epoch) &&
        uimg_load(j->cache, &j->im) == 0) {
        j->cached = 1;
        return 0;
    }

    // Sniff before loading: uimg_load() reads the WHOLE file, and most
    // files in a directory are not images.
    if (got < 4) return -1;
    if (!uimg_probe(head, got)) {
        if (video != 1) return -1;
        // A VIDEO's picture is a frame a tenth of the way in (at most
        // 3 s): the first is black as often as not, a fade's start.
        uint32_t at = in.ms / 10;
        if (at > 3000) at = 3000;
        return uvid_still(j->path, at, j->px, j->px, &j->im) == 0 ? 0 : -1;
    }

    struct uimg full;
    if (uimg_load(j->path, &full) != 0) return -1;  // broken or refused
    int tw, th;
    uimg_fit_size(full.w, full.h, j->px, j->px, UIMG_FIT_CONTAIN, &tw, &th);
    int rc = uimg_scale(&full, tw, th, &j->im);
    uimg_free(&full);
    return rc != 0 ? -1 : 0;   // the WRITE is the main thread's -- see uthumb_posted()
}

// **ONE WORKER FOR THE PROCESS'S LIFE, NEVER ONE PER DECODE.** A thread
// per thumbnail is the obvious shape and it degrades the whole machine
// here: a detached thread's stack reclaim still SPINS AND YIELDS
// (docs/roadmap.md), so every finished decode leaves something burning a
// scheduler slot. Measured: filemanager_test.py went from 1 failure to
// 22 with a thread per decode and back with this.
//
// IT PARKS BY SLEEPING: pthread_cond_wait spins on sys_yield() here. The
// back-off is what keeps an idle app off the CPU while a drain still
// streams -- a fixed slow poll would be the rate limit this replaced.
#define THUMB_POLL_MS 5
#define THUMB_IDLE_MS 100
#define THUMB_BUSY_POLLS 200   // ~1s of fast polling after a job

static pthread_t g_worker;
static int g_worker_started;
static volatile int g_job_ready;   // the worker may take g_job

static void *thumb_main(void *arg) {
    (void)arg;
    int idle = 0;
    for (;;) {
        if (!g_job_ready) {
            sys_sleep_ms(idle++ > THUMB_BUSY_POLLS ? THUMB_IDLE_MS : THUMB_POLL_MS);
            continue;
        }
        idle = 0;
        // PUBLISHED BEFORE THE FLAG by the main thread, and read after
        // it here -- the store order x86 gives is what makes this safe
        // without a lock.
        struct thumb_job *j = g_job;
        j->rc = thumb_produce(j);
        g_job_ready = 0;
        wake();
    }
    return NULL;
}

// --- the table, all of it on the main thread ----------------------------

static struct thumb *thumb_find(const char *path, int px) {
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].state != THUMB_EMPTY && g_thumbs[i].px == px &&
            strcmp(g_thumbs[i].path, path) == 0)
            return &g_thumbs[i];
    return 0;
}

static struct thumb *thumb_slot(const char *path, int px) {
    struct thumb *t = thumb_find(path, px);
    if (t) return t;
    struct thumb *lru = &g_thumbs[0];
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].used < lru->used) lru = &g_thumbs[i];
    uimg_free(&lru->im); // safe on a never-decoded image (uimg.h)
    memset(lru, 0, sizeof *lru);
    strlcpy(lru->path, path, sizeof lru->path);
    return lru;
}

// Start the most recently WANTED pending entry -- "wanted" is the LRU
// clock the lookup stamps, so what is on screen decodes first.
static void thumb_start_next(void) {
    if (g_job) return;
    struct thumb *pick = 0;
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].state == THUMB_PENDING &&
            (!pick || g_thumbs[i].used > pick->used))
            pick = &g_thumbs[i];
    if (!pick) return;

    struct thumb_job *j = calloc(1, sizeof *j);
    if (!j) { pick->state = THUMB_NOT_IMAGE; return; }
    strlcpy(j->path, pick->path, sizeof j->path);
    j->px = pick->px;
    j->src_epoch = cal_rtc_to_epoch(&pick->mtime);
    if (!cache_path(pick->path, pick->px, j->cache, sizeof j->cache))
        j->cache[0] = '\0';
    pick->state = THUMB_DECODING;
    if (!g_batch) g_batch_t0 = sys_monotonic_ns();

    // **PUBLISHED BEFORE THE FLAG**, which is what the worker reads.
    g_job = j;

    if (!g_worker_started) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        g_worker_started = pthread_create(&g_worker, &at, thumb_main, 0) == 0;
        if (!g_worker_started) {
            // NO THREAD IS NOT NO THUMBNAIL: decode here instead. The
            // window freezes for it, which beats refusing the file.
            j->rc = thumb_produce(j);
            wake();
            return;
        }
    }
    g_job_ready = 1;
}

// The slot a finished job belongs to, or NULL if it was evicted or
// re-asked at a different size while the worker ran.
static struct thumb *thumb_owner(const struct thumb_job *j) {
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].state == THUMB_DECODING && g_thumbs[i].px == j->px &&
            strcmp(g_thumbs[i].path, j->path) == 0)
            return &g_thumbs[i];
    return 0;
}

const struct uimg *uthumb_get(const char *path, const struct rtc_time *mtime,
                              uint32_t size, int px) {
    if (!path || !mtime || px <= 0) return 0;
    struct thumb *t = thumb_slot(path, px);
    t->used = ++g_thumb_clock;

    int stale = t->state == THUMB_EMPTY || t->px != px || t->fsize != size ||
                memcmp(&t->mtime, mtime, sizeof t->mtime) != 0;
    if (stale) {
        t->mtime = *mtime;
        t->fsize = size;
        t->px = px;
        uimg_free(&t->im);
        t->state = THUMB_PENDING;
        // WAKE THE LOOP, don't wait for a tick. This runs inside a draw,
        // so the post is handled the moment the frame is done. One at a
        // time: a cell per visible icon would queue forty of these per
        // frame to say the one thing.
        if (!g_job && !g_kick_posted)
            g_kick_posted = wake();
    }
    return t->state == THUMB_READY ? &t->im : 0;
}

const char *uthumb_note(const char *path, int px) {
    const struct thumb *t = thumb_find(path, px);
    return t && t->state == THUMB_READY && t->note[0] ? t->note : 0;
}

int uthumb_failed(const char *path, int px) {
    const struct thumb *t = thumb_find(path, px);
    return t && t->state == THUMB_NOT_IMAGE;
}

int uthumb_posted(void) {
    g_kick_posted = 0;
    int changed = 0;

    struct thumb_job *j = (g_job && !g_job_ready) ? g_job : 0;
    if (j) {
        g_job = NULL;
        struct thumb *t = thumb_owner(j);
        if (!t) {
            uimg_free(&j->im);          // nobody is waiting for it now
        } else if (j->rc == 0) {
            t->im = j->im;
            strlcpy(t->note, j->note, sizeof t->note);
            t->state = THUMB_READY;
            changed = 1;
            // THE CACHE WRITE HAPPENS HERE, ON THE MAIN THREAD, with the
            // `stored` hook in the same step: an app that watches the
            // filesystem generation claims its own write there, and from
            // the worker the counter could move before the post arrived.
            if (!j->cached && j->cache[0]) {
                cache_store(j->cache, &j->im);
                if (g_cfg.stored) g_cfg.stored(g_cfg.ctx);
            }
        } else {
            t->state = THUMB_NOT_IMAGE; // broken or refused: the caller's icon
        }
        g_batch++;
        g_batch_cached += (unsigned)j->cached;
        free(j);
    }
    thumb_start_next();
    // The queue is empty: say what the drain cost, ONCE.
    if (!g_job && g_batch) {
        ulogf("%s: %u thumbnail(s) in %llu ms, %u from the cache\n",
              g_cfg.log_prefix ? g_cfg.log_prefix : "thumbs",
              g_batch, (sys_monotonic_ns() - g_batch_t0) / 1000000ull,
              g_batch_cached);
        g_batch = g_batch_cached = 0;
    }
    return changed;
}

int uthumb_tick(void) {
    thumb_start_next();
    return 0;
}
