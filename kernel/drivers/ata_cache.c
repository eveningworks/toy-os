// The write-back sector cache. See kernel/include/kernel/ata_cache.h
// for why it lives under the ATA driver rather than in the block layer,
// and for the invariant the whole thing rests on (a flush that cannot
// report failure cannot keep a journal barrier honest).
#include "ata_cache.h"
#include "ata.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "timer.h" // pit_ticks
#include <stddef.h>

// driver-none: the write-back cache above the ATA driver

// One line per sector. `lba` is only meaningful while `valid`.
struct atac_line {
    uint32_t lba;
    uint8_t valid;
    uint8_t dirty;
    uint16_t age;    // LRU stamp within the set; higher is more recent
};

static struct atac_line g_line[ATAC_LINES];
static uint8_t g_data[ATAC_LINES][ATA_SECTOR_SIZE];

static const struct atac_ops *g_ops;
static int g_enabled;
static struct atac_stats g_stats;

static uint16_t g_clock;          // monotonic LRU stamp
static uint64_t g_last_write_tick;
static uint32_t g_dirty;          // maintained, not recounted per write

// Re-entrancy guard. atac_idle() is reached from scheduler_idle(),
// which any waiting loop may call -- including one reached from inside
// a disk operation. Starting a write-back on top of a transfer already
// in flight would interleave two commands at one drive; refusing is the
// only safe answer, and costs nothing because the next idle call
// retries.
static int g_busy;

static uint32_t set_of(uint32_t lba) { return lba % ATAC_SETS; }

static uint32_t index_of(const struct atac_line *l) {
    return (uint32_t)(l - g_line);
}

// The line holding `lba`, or NULL.
static struct atac_line *find(uint32_t lba) {
    uint32_t base = set_of(lba) * ATAC_WAYS;
    for (uint32_t w = 0; w < ATAC_WAYS; w++) {
        struct atac_line *l = &g_line[base + w];
        if (l->valid && l->lba == lba) return l;
    }
    return NULL;
}

static void mark_dirty(struct atac_line *l) {
    if (!l->dirty) { l->dirty = 1; g_dirty++; }
}

static void mark_clean(struct atac_line *l) {
    if (l->dirty) { l->dirty = 0; g_dirty--; }
}

static void drop_line(struct atac_line *l) {
    if (l->valid) { mark_clean(l); l->valid = 0; }
}

static int write_back(struct atac_line *l) {
    if (!l->valid || !l->dirty) return 1;
    if (!g_ops->write(l->lba, 1, g_data[index_of(l)])) {
        // The line STAYS dirty. Dropping it would turn a reported drive
        // failure into silently lost data, which is the one outcome this
        // cache must never produce -- the caller can retry, and a flush
        // that returns 0 lets the filesystem leave its journal committed
        // for replay instead of believing the write landed.
        g_stats.failed_writebacks++;
        return 0;
    }
    mark_clean(l);
    g_stats.writebacks++;
    return 1;
}

// A victim in `lba`'s set: a free way first, else the least recently
// used, written back before it is handed over. NULL if the only
// candidate is dirty and its write-back failed -- refusing to reuse it
// is what keeps the data recoverable.
static struct atac_line *evict_for(uint32_t lba) {
    uint32_t base = set_of(lba) * ATAC_WAYS;
    struct atac_line *lru = &g_line[base];
    for (uint32_t w = 0; w < ATAC_WAYS; w++) {
        struct atac_line *l = &g_line[base + w];
        if (!l->valid) return l;
        // Unsigned wraparound on `age` is deliberate and harmless: it
        // can only mis-rank two lines within one set, which costs a
        // suboptimal eviction and never correctness.
        if ((uint16_t)(g_clock - l->age) > (uint16_t)(g_clock - lru->age)) lru = l;
    }
    if (lru->dirty && !write_back(lru)) return NULL;
    g_stats.evictions++;
    drop_line(lru);
    return lru;
}

static void touch(struct atac_line *l) { l->age = ++g_clock; }

void atac_init(const struct atac_ops *ops) {
    if (!ops || !ops->read || !ops->write || !ops->flush) {
        klog_write(KLOG_ERR "atac: refused ops missing an operation -- cache off\n");
        return;
    }
    g_ops = ops;
    k_memset(g_line, 0, sizeof g_line);
    g_dirty = 0;
    g_enabled = 1;
    klog_printf("atac: write-back cache on (%u sectors, %u KiB, %u-way)\n",
                (unsigned)ATAC_LINES,
                (unsigned)(ATAC_LINES * ATA_SECTOR_SIZE / 1024),
                (unsigned)ATAC_WAYS);
}

int atac_enabled(void) { return g_enabled && g_ops != NULL; }

void atac_set_enabled(int on) {
    if (!on && atac_enabled()) atac_flush();
    g_enabled = on ? 1 : 0;
}

// --- the bypass path --------------------------------------------------
//
// A transfer too large to cache still has to be COHERENT with what is
// cached, in both directions. Before a raw read, overlapping dirty
// lines are written back, or the read returns superseded data. Before a
// raw write, overlapping lines are dropped, or a later write-back of a
// stale line silently overwrites what was just written. Neither failure
// announces itself.
static int reconcile_range(uint32_t lba, int count, int for_write) {
    int ok = 1;
    for (int i = 0; i < count; i++) {
        struct atac_line *l = find(lba + (uint32_t)i);
        if (!l) continue;
        if (l->dirty && !write_back(l)) { ok = 0; continue; }
        if (for_write) drop_line(l);
    }
    return ok;
}

int atac_read(uint32_t lba, int count, void *buf) {
    if (!atac_enabled()) return g_ops ? g_ops->read(lba, count, buf) : 0;

    uint8_t *dst = (uint8_t *)buf;

    if (count > ATAC_MAX_LINES) {
        if (!reconcile_range(lba, count, 0)) return 0;
        g_busy = 1;
        int r = g_ops->read(lba, count, dst);
        g_busy = 0;
        return r;
    }

    // Serve what is cached; fetch the rest in CONTIGUOUS RUNS rather
    // than one sector at a time -- a per-sector fetch would turn one
    // 8-sector miss into eight command dispatches, and on this
    // controller a dispatch costs far more than the copy it saves.
    for (int i = 0; i < count;) {
        struct atac_line *l = find(lba + (uint32_t)i);
        if (l) {
            k_memcpy(dst + (uint32_t)i * ATA_SECTOR_SIZE, g_data[index_of(l)],
                     ATA_SECTOR_SIZE);
            touch(l);
            g_stats.hits++;
            i++;
            continue;
        }
        int run = 0;
        while (i + run < count && !find(lba + (uint32_t)(i + run))) run++;

        g_busy = 1;
        int r = g_ops->read(lba + (uint32_t)i, run,
                            dst + (uint32_t)i * ATA_SECTOR_SIZE);
        g_busy = 0;
        if (!r) return 0;

        for (int k = 0; k < run; k++) {
            g_stats.misses++;
            struct atac_line *nl = evict_for(lba + (uint32_t)(i + k));
            if (!nl) continue;   // no way could be freed; serve this one uncached
            nl->lba = lba + (uint32_t)(i + k);
            nl->valid = 1;
            k_memcpy(g_data[index_of(nl)],
                     dst + (uint32_t)(i + k) * ATA_SECTOR_SIZE, ATA_SECTOR_SIZE);
            touch(nl);
        }
        i += run;
    }
    return 1;
}

int atac_write(uint32_t lba, int count, const void *buf) {
    if (!atac_enabled()) return g_ops ? g_ops->write(lba, count, buf) : 0;

    const uint8_t *src = (const uint8_t *)buf;
    g_last_write_tick = pit_ticks();

    if (count > ATAC_MAX_LINES) {
        if (!reconcile_range(lba, count, 1)) return 0;
        g_busy = 1;
        int r = g_ops->write(lba, count, src);
        g_busy = 0;
        return r;
    }

    for (int i = 0; i < count; i++) {
        uint32_t s = lba + (uint32_t)i;
        const uint8_t *from = src + (uint32_t)i * ATA_SECTOR_SIZE;
        struct atac_line *l = find(s);
        if (!l) {
            l = evict_for(s);
            if (!l) {
                // No way could be freed (its victim's write-back
                // failed). Go straight to the device rather than
                // pretending the write happened.
                g_busy = 1;
                int r = g_ops->write(s, 1, from);
                g_busy = 0;
                if (!r) return 0;
                continue;
            }
            l->lba = s;
            l->valid = 1;
        }
        // A whole sector is overwritten, so a miss needs no read-fill:
        // there is no part of the line the new data does not cover.
        k_memcpy(g_data[index_of(l)], from, ATA_SECTOR_SIZE);
        mark_dirty(l);
        touch(l);
        g_stats.writes++;
    }

    // Bound how much can sit dirty in RAM. Without this a burst of
    // metadata writes accumulates until something else happens to
    // flush.
    if (g_dirty >= ATAC_DIRTY_HIGH) atac_flush();
    return 1;
}

int atac_flush(void) {
    if (!atac_enabled()) return g_ops && g_ops->flush ? g_ops->flush() : 1;
    if (g_busy) return 1;   // a transfer is already in flight; see g_busy
    g_busy = 1;
    int ok = 1;
    for (uint32_t i = 0; i < ATAC_LINES; i++) {
        if (g_line[i].valid && g_line[i].dirty && !write_back(&g_line[i])) ok = 0;
    }
    // The device's own flush goes LAST and unconditionally: even if a
    // write-back failed, the ones that succeeded still have to reach the
    // platter, and skipping it would leave them in the drive's cache.
    if (!g_ops->flush()) ok = 0;
    g_stats.flushes++;
    g_busy = 0;
    if (!ok) klog_write(KLOG_ERR "atac: FLUSH FAILED -- dirty sectors kept; the caller must "
                        "NOT treat this as durable\n");
    return ok;
}

int atac_drop(void) {
    int ok = atac_flush();
    // Only clean lines are dropped. A line still dirty is one whose
    // write-back failed, and forgetting it is exactly the silent loss
    // this cache is built to avoid -- it is kept so a later flush (or
    // `sync`) can still try.
    for (uint32_t i = 0; i < ATAC_LINES; i++) {
        if (!g_line[i].dirty) g_line[i].valid = 0;
    }
    return ok;
}

void atac_idle(void) {
    if (!atac_enabled() || g_busy || g_dirty == 0) return;
    if (pit_ticks() - g_last_write_tick < ATAC_IDLE_TICKS) return;
    atac_flush();
}

void atac_get_stats(struct atac_stats *out) {
    if (!out) return;
    *out = g_stats;
    out->dirty = g_dirty;
    out->valid = 0;
    for (uint32_t i = 0; i < ATAC_LINES; i++) {
        if (g_line[i].valid) out->valid++;
    }
}
