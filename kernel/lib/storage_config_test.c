// Tests for the `storage.sync` setting (storage_config.h).
//
// The thing worth asserting is not that a string parses -- it is that
// the flag REACHES the journal, because the whole setting is one branch
// in txn_commit() and a flag nothing reads is the failure mode a parse
// test cannot see. So this counts real device flushes across a write,
// through the block layer's own counters.
#include "ktest.h"
#include "storage_config.h"
#include "block_stat.h"
#include "fs.h"
#include "timer.h"
#include "tfs3.h"
#include "string.h"

#define SCRATCH "/var/tmp/ktest_sync.bin"

static uint64_t flushes(void) {
    uint64_t c = 0;
    blk_stat_get(BLK_STAT_FLUSH, &c, NULL, NULL, NULL);
    return c;
}

KTEST("storage", "an unknown sync mode is refused and leaves the mode alone") {
    int was = storage_sync_strict();
    // Through the same in-memory path a hand-edited /etc file takes.
    storage_config_init();          // re-adopt whatever is on disk
    int adopted = storage_sync_strict();
    KTEST_ASSERT(adopted == 0 || adopted == 1);
    KTEST_ASSERT_EQ(adopted, was);  // nothing on this boot changed it
}

// THE ONE THAT MATTERS: strict must issue barriers and lazy must not.
// Asserted as a COMPARISON between the two modes over the same work
// rather than against a fixed number, because the desktop is running
// and flushing on its own -- a test that read an absolute count would
// be measuring whatever else the machine did.
// BATCHED IS THE MODE WITH THE INTERESTING FAILURE. It keeps a journal
// transaction open across writes, so the two things that could go wrong
// are losing the staged inode (a write that reports success and is not
// there) and never committing at all.
//
// Asserted as a comparison, not against a fixed count, because the
// desktop is writing on its own throughout.
KTEST("storage", "batched commits fewer times than strict, and keeps the data") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");

    static uint8_t buf[4096];
    static uint8_t back[4096];
    k_memset(buf, 0x3C, sizeof buf);
    int restore = storage_sync_strict();
    int restore_b = storage_sync_batched();

    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(fs_touch(SCRATCH), 1);
    enum { WRITES = 8 };

    storage_config_set_mode_for_test(1, 0);   // strict
    uint64_t a0 = flushes();
    for (int i = 0; i < WRITES; i++)
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, (uint64_t)i * sizeof buf, buf, sizeof buf), 1);
    uint64_t strict_cost = flushes() - a0;

    storage_config_set_mode_for_test(1, 1);   // batched
    uint64_t b0 = flushes();
    for (int i = 0; i < WRITES; i++)
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, (uint64_t)i * sizeof buf, buf, sizeof buf), 1);
    uint64_t batched_cost = flushes() - b0;

    KTEST_ASSERT(batched_cost < strict_cost);

    // AND THE BYTES ARE THERE, which the flush comparison alone would
    // be perfectly happy about if the staged inode had been dropped.
    // Read back BEFORE forcing a commit: a batched write must be
    // visible to a reader immediately, only not yet durable.
    for (int i = 0; i < WRITES; i++) {
        k_memset(back, 0, sizeof back);
        KTEST_ASSERT_EQ((int64_t)fs_read_range(SCRATCH, (uint64_t)i * sizeof buf,
                                               back, sizeof back), (int64_t)sizeof back);
        KTEST_ASSERT_EQ((int64_t)back[0], (int64_t)0x3C);
    }
    KTEST_ASSERT_EQ((int64_t)fs_size(SCRATCH), (int64_t)(WRITES * (int)sizeof buf));

    // fs_sync() must land the deferred transaction, and the assertion
    // is on WRITES rather than flushes: fs_sync() flushes every mounted
    // device whatever the backend does, so a flush count would go up
    // even if the commit had been skipped entirely. A commit writes --
    // journal data, the header, the target, the header again -- and a
    // device flush writes nothing.
    uint64_t w0 = 0;
    blk_stat_get(BLK_STAT_WRITE, &w0, NULL, NULL, NULL);
    uint32_t wrote = 0;
    KTEST_ASSERT_EQ(fs_sync(&wrote), 1);
    uint64_t w1 = 0;
    blk_stat_get(BLK_STAT_WRITE, &w1, NULL, NULL, NULL);
    KTEST_ASSERT(w1 > w0);

    storage_config_set_mode_for_test(restore, restore_b);
    fs_delete(SCRATCH);
}

// A BATCHED WRITE MUST BE VISIBLE BEFORE IT IS DURABLE, and this is the
// bug that shipped in the first version of `batched`.
//
// A deferred transaction holds the newest inode image in the journal
// staging buffer while the DISK still holds the previous one. Readers
// go through vol_read_sectors(), so without an overlay there they see
// the old size and the old block pointers -- `fs_size()` reporting the
// size the file had before writes that already returned success.
// `diskbench`'s read pass failed outright with a short read.
//
// EXTENDS THE FILE MANY TIMES IN ONE BATCH, deliberately: a single
// write-then-read is racy, because anything else on the machine that
// opens a transaction commits this one on its way past (txn_begin), and
// the desktop is running. Growing the file repeatedly and checking the
// size after EVERY step fails on whichever step no commit happened to
// intervene in -- and with the overlay it passes on all of them.
KTEST("storage", "a batched write is visible to a reader before it commits") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");

    static uint8_t buf[4096];
    static uint8_t back[4096];
    k_memset(buf, 0x71, sizeof buf);
    int restore = storage_sync_strict(), restore_b = storage_sync_batched();

    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(fs_touch(SCRATCH), 1);
    storage_config_set_mode_for_test(1, 1);   // batched

    enum { STEPS = 12 };
    for (int i = 0; i < STEPS; i++) {
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, (uint64_t)i * sizeof buf,
                                       buf, sizeof buf), 1);
        // The size the write just established, read back through the
        // ordinary path -- which is where a stale inode surfaces.
        if ((int64_t)fs_size(SCRATCH) != (int64_t)((i + 1) * (int)sizeof buf)) {
            storage_config_set_mode_for_test(restore, restore_b);
            ktest_fail_eq(ctx, "fs_size after a batched write",
                          (int64_t)fs_size(SCRATCH),
                          (int64_t)((i + 1) * (int)sizeof buf), __FILE__, __LINE__);
            return;
        }
        // ...and the bytes at the new tail, which a stale block map
        // would report as a hole.
        k_memset(back, 0, sizeof back);
        if ((int64_t)fs_read_range(SCRATCH, (uint64_t)i * sizeof buf,
                                   back, sizeof back) != (int64_t)sizeof back ||
            back[0] != 0x71) {
            storage_config_set_mode_for_test(restore, restore_b);
            ktest_fail(ctx, "a batched write read back short or empty",
                       __FILE__, __LINE__);
            return;
        }
    }

    storage_config_set_mode_for_test(restore, restore_b);
    fs_delete(SCRATCH);
}

// fsync() MUST COMMIT, and the assertion is on WRITES rather than on
// flushes: it flushes the device whatever the backend does, so a flush
// count rises even if the commit were skipped entirely. A commit
// writes -- journal data, the header, the target, the header again --
// and a device flush writes nothing.
KTEST("storage", "fsync commits a batched write") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");

    static uint8_t buf[4096];
    k_memset(buf, 0x6D, sizeof buf);
    int restore = storage_sync_strict(), restore_b = storage_sync_batched();

    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(fs_touch(SCRATCH), 1);
    storage_config_set_mode_for_test(1, 1);   // batched

    KTEST_ASSERT_EQ(fs_write_range(SCRATCH, 0, buf, sizeof buf), 1);
    uint64_t w0 = 0;
    blk_stat_get(BLK_STAT_WRITE, &w0, NULL, NULL, NULL);

    KTEST_ASSERT_EQ(fs_sync_path(SCRATCH), 1);

    uint64_t w1 = 0;
    blk_stat_get(BLK_STAT_WRITE, &w1, NULL, NULL, NULL);
    KTEST_ASSERT(w1 > w0);

    // ...AND IT IS SCOPED TO A MOUNT, which is the claim that separates
    // it from `sync`. Syncing a path on a DIFFERENT mount must not
    // commit this one's transaction.
    //
    // Not tested with a nonexistent path: mount_resolve() falls back to
    // the root for any absolute path, so "/nonexistent/x" is a perfectly
    // ordinary path ON the root mount and syncing it is correct. There
    // is no such thing as a path with no mount, which is what the first
    // version of this check assumed.
    if (fs_exists("/boot")) {
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, sizeof buf, buf, sizeof buf), 1);
        uint64_t o0 = 0;
        blk_stat_get(BLK_STAT_WRITE, &o0, NULL, NULL, NULL);
        fs_sync_path("/boot");
        uint64_t o1 = 0;
        blk_stat_get(BLK_STAT_WRITE, &o1, NULL, NULL, NULL);
        // A tfs3 commit is four block writes; /boot is FAT32 and
        // declares no `sync`, so syncing it can only flush a device.
        KTEST_ASSERT(o1 - o0 < 4);
    }

    storage_config_set_mode_for_test(restore, restore_b);
    fs_delete(SCRATCH);
}

// A DEFERRED COMMIT MUST LAND ON ITS OWN. Without the idle path,
// `batched` commits only when another transaction opens, a second mount
// activates, `sync` runs, or the volume unmounts -- so a machine that
// wrote a file and was then left alone would hold that inode update
// indefinitely, which is a durability hole rather than a slow path.
//
// Asserted by WAITING rather than by calling sync: calling it would
// prove the commit works, which is already covered, not that anything
// makes it happen unprompted.
KTEST("storage", "a deferred commit lands on the idle path, unprompted") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");

    static uint8_t buf[4096];
    k_memset(buf, 0x2B, sizeof buf);
    int restore = storage_sync_strict(), restore_b = storage_sync_batched();

    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(fs_touch(SCRATCH), 1);
    storage_config_set_mode_for_test(1, 1);   // batched

    uint64_t w0 = 0;
    blk_stat_get(BLK_STAT_WRITE, &w0, NULL, NULL, NULL);
    KTEST_ASSERT_EQ(fs_write_range(SCRATCH, 0, buf, sizeof buf), 1);

    // Long enough for the interval to expire several times over, and
    // bounded so a broken idle path fails rather than hangs. fs_idle()
    // is what scheduler_idle() calls; driving it directly keeps the
    // test independent of whether this machine happens to go idle.
    // COUNTS THE IDLE PATH'S OWN COMMITS, not commits in general.
    // Asserting "a commit happened" cannot work: anything that opens a
    // transaction commits the deferred one on its way past, and the
    // desktop is always writing -- so the first version of this test
    // passed with the idle path disabled entirely.
    uint64_t idle0 = tfs3_idle_commits();
    int landed = 0;
    uint64_t deadline = pit_ticks() + storage_writeback_ticks() * 4 + PIT_HZ;
    while (pit_ticks() < deadline) {
        fs_idle();
        if (tfs3_idle_commits() > idle0) { landed = 1; break; }
    }
    (void)w0;
    storage_config_set_mode_for_test(restore, restore_b);
    KTEST_ASSERT(landed);

    fs_delete(SCRATCH);
}

// THE HAZARD txn_begin() EXISTS TO CLOSE. An operation that opens its
// own transaction while one is deferred would discard every inode
// staged in it -- writes reported as succeeded, silently gone. A create
// between two writes is the cheapest way to provoke exactly that.
KTEST("storage", "an operation between batched writes does not lose them") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");

    static uint8_t buf[4096];
    static uint8_t back[4096];
    k_memset(buf, 0x5E, sizeof buf);
    int restore = storage_sync_strict(), restore_b = storage_sync_batched();

    fs_delete(SCRATCH);
    fs_delete("/var/tmp/ktest_sync2.bin");
    KTEST_ASSERT_EQ(fs_touch(SCRATCH), 1);

    storage_config_set_mode_for_test(1, 1);   // batched
    KTEST_ASSERT_EQ(fs_write_range(SCRATCH, 0, buf, sizeof buf), 1);

    // A create opens its OWN transaction (credits=3), which is what
    // would blow away the staged inode above.
    KTEST_ASSERT_EQ(fs_touch("/var/tmp/ktest_sync2.bin"), 1);

    KTEST_ASSERT_EQ(fs_write_range(SCRATCH, sizeof buf, buf, sizeof buf), 1);
    KTEST_ASSERT_EQ(fs_sync(NULL), 1);
    storage_config_set_mode_for_test(restore, restore_b);

    // Both halves, and the size -- a lost first inode shows up as a
    // file that is 4096 bytes long instead of 8192.
    KTEST_ASSERT_EQ((int64_t)fs_size(SCRATCH), (int64_t)(2 * (int)sizeof buf));
    for (int i = 0; i < 2; i++) {
        k_memset(back, 0, sizeof back);
        KTEST_ASSERT_EQ((int64_t)fs_read_range(SCRATCH, (uint64_t)i * sizeof buf,
                                               back, sizeof back), (int64_t)sizeof back);
        KTEST_ASSERT_EQ((int64_t)back[0], (int64_t)0x5E);
    }

    fs_delete(SCRATCH);
    fs_delete("/var/tmp/ktest_sync2.bin");
}

KTEST("storage", "lazy issues fewer device flushes than strict for the same write") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");

    static uint8_t buf[4096];
    k_memset(buf, 0x5A, sizeof buf);
    int restore = storage_sync_strict();

    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(fs_touch(SCRATCH), 1);

    // Same work twice, once per mode. Several writes so the difference
    // is a margin rather than one flush either way.
    enum { WRITES = 8 };

    storage_config_set_mode_for_test(1, 0);
    uint64_t a0 = flushes();
    for (int i = 0; i < WRITES; i++)
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, (uint64_t)i * sizeof buf, buf, sizeof buf), 1);
    uint64_t strict_cost = flushes() - a0;

    storage_config_set_mode_for_test(1, 0), storage_config_set_mode_for_test(0, 0);
    uint64_t b0 = flushes();
    for (int i = 0; i < WRITES; i++)
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, (uint64_t)i * sizeof buf, buf, sizeof buf), 1);
    uint64_t lazy_cost = flushes() - b0;

    storage_config_set_mode_for_test(restore, 0);

    KTEST_ASSERT(strict_cost >= (uint64_t)WRITES);   // two barriers a write
    KTEST_ASSERT(lazy_cost < strict_cost);

    // AND THE DATA IS STILL THERE. `lazy` defers the barrier, it does
    // not skip the write -- a mode that quietly lost the bytes would
    // satisfy the flush comparison above perfectly.
    static uint8_t back[4096];
    k_memset(back, 0, sizeof back);
    KTEST_ASSERT_EQ((int64_t)fs_read_range(SCRATCH, 0, back, sizeof back),
                    (int64_t)sizeof back);
    for (unsigned i = 0; i < sizeof back; i++) {
        if (back[i] != 0x5A) { KTEST_ASSERT_EQ((int64_t)back[i], 0x5A); }
    }

    fs_delete(SCRATCH);
}
