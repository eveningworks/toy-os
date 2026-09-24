// Tests for the TFS3 backend driven DIRECTLY, over RAM volumes of their
// own -- the fat32_test.c shape. fs_test.c covers TFS3 through fs_* on
// the real root; what it cannot build is TWO TFS3 volumes side by side,
// which is what these need.
//
// Each volume is 4 MiB: TFS3 refuses anything smaller than one partial
// group (T3_MIN_GROUP_BLOCKS past group0), and this is the next round
// size above it.
#include "ktest.h"
#include "tfs3.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "mount.h"  // mount_scratch_begin/end, mount_add -- each volume owns its state
#include "fs.h"     // fs_sync_path(), fs_lock_held_at()

#define VOL_SECTORS 8192u           // 4 MiB
#define VOL_BYTES   (VOL_SECTORS * 512u)

static uint8_t *g_vol[2];

static uint32_t vol_count(void) { return VOL_SECTORS; }

static int vol_rw(int v, uint32_t lba, int count, void *rd, const void *wr) {
    if (!g_vol[v] || count <= 0) return 0;
    if ((uint64_t)lba + (uint64_t)count > VOL_SECTORS) return 0;
    uint8_t *at = g_vol[v] + (uint64_t)lba * 512;
    if (rd) k_memcpy(rd, at, (uint32_t)count * 512);
    else k_memcpy(at, wr, (uint32_t)count * 512);
    return 1;
}
static int v0_read(uint32_t lba, int n, void *b)        { return vol_rw(0, lba, n, b, 0); }
static int v0_write(uint32_t lba, int n, const void *b) { return vol_rw(0, lba, n, 0, b); }
static int v1_read(uint32_t lba, int n, void *b)        { return vol_rw(1, lba, n, b, 0); }
static int v1_write(uint32_t lba, int n, const void *b) { return vol_rw(1, lba, n, 0, b); }
static int vol_xfer(void) { return 8; }

static const struct block_device VOL_DEV[2] = {
    { .name = "t3test0", .sector_count = vol_count, .read_sectors = v0_read,
      .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer },
    { .name = "t3test1", .sector_count = vol_count, .read_sectors = v1_read,
      .write_sectors = v1_write, .max_sectors_per_xfer = vol_xfer },
};

static const struct fs_ops *T(void) { return &tfs3_ops; }

// Block `i` of a file on volume `v` says which volume and which block it
// is, in every byte -- so a read served from the other volume's
// metadata cannot come back looking right.
static void fill(uint8_t *b, int v, int i) {
    for (int k = 0; k < 4096; k++) b[k] = (uint8_t)(0x40 + v * 0x20 + i);
}

// A POINTER TABLE AT THE SAME BLOCK NUMBER ON TWO VOLUMES, WITH DIFFERENT
// CONTENTS. Both files take blocks 0-11 directly, so the single-indirect
// table lands on the same block number on both; volume 0's table maps
// index 12, volume 1's leaves 12 a hole and maps 13. Reading volume 0's
// block 12 loads its table into the read-side pointer cache; reading
// volume 1's block 13 straight after, with no write between to drop the
// cache, must use volume 1's table. When that cache was file-scope it
// used volume 0's -- where index 13 is a hole -- and returned zeros.
KTEST("tfs3", "a pointer table cached for one volume is not served to another") {
    struct fs_scratch sc[2];
    int have[2] = {0, 0};
    for (int v = 0; v < 2; v++) {
        if (!g_vol[v]) g_vol[v] = kmalloc(VOL_BYTES);
        if (!g_vol[v]) break;
        k_memset(g_vol[v], 0, VOL_BYTES);
        if (!mount_scratch_begin(T(), &sc[v])) break;
        have[v] = 1;
    }
    if (!have[0] || !have[1]) {
        for (int v = 0; v < 2; v++) if (have[v]) mount_scratch_end(&sc[v]);
        KTEST_SKIP("could not allocate two 4 MiB volumes");
    }

    static uint8_t blk[4096], back[4096];
    int ok = 1;
    for (int v = 0; v < 2 && ok; v++) {
        void *st = sc[v].st;
        ok = T()->format(st, &VOL_DEV[v]) && T()->init(st, &VOL_DEV[v], 0) >= 0;
        for (int i = 0; ok && i < 12; i++) {
            fill(blk, v, i);
            ok = T()->write_range(st, "/f", (uint64_t)i * 4096, blk, 4096);
        }
        int last = v == 0 ? 12 : 13;           // volume 1 leaves 12 a hole
        fill(blk, v, last);
        if (ok) ok = T()->write_range(st, "/f", (uint64_t)last * 4096, blk, 4096);
    }
    KTEST_ASSERT(ok);

    uint32_t got0 = T()->read_range(sc[0].st, "/f", 12u * 4096, back, 4096);
    fill(blk, 0, 12);
    int same0 = got0 == 4096 && k_memcmp(back, blk, 4096) == 0;

    uint32_t got1 = T()->read_range(sc[1].st, "/f", 13u * 4096, back, 4096);
    fill(blk, 1, 13);
    int same1 = got1 == 4096 && k_memcmp(back, blk, 4096) == 0;
    uint8_t first1 = back[0];

    for (int v = 1; v >= 0; v--) mount_scratch_end(&sc[v]);
    for (int v = 0; v < 2; v++) { kfree(g_vol[v]); g_vol[v] = 0; }

    KTEST_ASSERT(same0);
    KTEST_ASSERT_EQ((int)first1, 0x40 + 0x20 + 13);   // not 0: the hole in volume 0's table
    KTEST_ASSERT(same1);
}

// ---- the flush happens under the mount's lock ----------------------

// A MOUNTED RAM volume whose flush() records whether its mount's lock is
// held at that moment. fs_exclusive_begin() promises its holder that
// nothing is at the disk, and the legacy `run` spins on any lock it then
// meets -- so a flush issued after the mount lock is released (fsync did
// this) is a hang waiting for a sleeping flusher. Both entry points are
// asked, because they took different routes to the device.
#define FLUSH_POINT "/var/tmp/.ktest_flushmnt"

static int g_flush_watch, g_flush_calls, g_flush_locked;

static int v0_flush(void) {
    if (g_flush_watch) {
        g_flush_calls++;
        if (fs_lock_held_at(FLUSH_POINT "/f")) g_flush_locked++;
    }
    return 1;
}

static const struct block_device FLUSH_DEV = {
    .name = "t3flush", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = v0_flush,
};

KTEST("tfs3", "fsync and sync flush the device while its mount is locked") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);

    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &FLUSH_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(FLUSH_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&FLUSH_DEV, "tfs3", FLUSH_POINT, 0, 0, &why);
    int wrote = mounted && fs_write(FLUSH_POINT "/f", "x", 0);

    g_flush_calls = g_flush_locked = 0;
    g_flush_watch = 1;
    int fsynced = wrote && fs_sync_path(FLUSH_POINT "/f");
    int fsync_calls = g_flush_calls, fsync_locked = g_flush_locked;
    g_flush_calls = g_flush_locked = 0;
    int synced = wrote && fs_sync(0);
    int sync_calls = g_flush_calls, sync_locked = g_flush_locked;
    g_flush_watch = 0;

    if (mounted) mount_remove(FLUSH_POINT, &why);
    fs_delete(FLUSH_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(mounted);
    KTEST_ASSERT(fsynced && synced);
    KTEST_ASSERT(fsync_calls >= 1);                 // the fixture reached the device
    KTEST_ASSERT_EQ(fsync_locked, fsync_calls);     // ...and every flush was locked
    KTEST_ASSERT(sync_calls >= 1);
    KTEST_ASSERT_EQ(sync_locked, sync_calls);
}
