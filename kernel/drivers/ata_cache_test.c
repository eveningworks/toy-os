// Tests for the write-back sector cache (kernel/ata_cache.h).
//
// The cache is an optimisation, so most of what could be asserted about
// it ("a hit is faster") is both hard to measure inside a live kernel
// and not what would hurt if it broke. What WOULD hurt is a lost write,
// so these concentrate on the three properties that stand between this
// cache and silent data loss:
//
//   1. What comes back is what went in -- including across the
//      cached/bypass boundary, which is the seam most likely to rot.
//   2. A flush FAILURE is reported. blkdev_flush(blk_root()) and ata_flush_now()
//      were `void` before this cache existed; TFS3's journal barriers
//      now depend on the answer, and a barrier that always says "yes"
//      is worse than no barrier at all.
//   3. Everything dirty is actually written, so `sync` and the
//      shutdown flush mean something.
//
// These run against the LIVE filesystem's disk, so they read and write
// through the ordinary fs API rather than poking sectors directly --
// scribbling on raw LBAs in a booted kernel would corrupt the volume
// the rest of the suite is using.
#include "ktest.h"
#include "tmppath.h"
#include "fs.h"
#include "ata.h"
#include "ata_cache.h"
#include "fs.h"
#include "string.h"
#include "fault_inject.h"

// Built from the configured directory rather than spelled out
// (api/tmppath.h), so moving scratch is a setting rather than a grep.
static const char *scratch_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_atac.bin");
    return p;
}
#define SCRATCH scratch_path()

KTEST("atac", "the cache is active on a machine with a disk") {
    // Not a tautology: atac_init() REFUSES an ops table missing an
    // operation, and a refusal is silent apart from one log line. If
    // this is ever red on a machine that has a drive, every other test
    // in this file is measuring the uncached path and passing anyway.
    if (!ata_present()) KTEST_SKIP("no drive attached");
    KTEST_ASSERT(ata_cache_active());
}

KTEST("atac", "a file round-trips through the cache unchanged") {
    if (!ata_present()) KTEST_SKIP("no drive attached");

    char buf[512];
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (char)(i * 7 + 3);

    fs_delete(SCRATCH);
    KTEST_ASSERT(fs_write_range(SCRATCH, 0, buf, sizeof buf) == 1);

    // Through a flush, so the data has to survive the write-back rather
    // than merely still being in the line it was written to.
    KTEST_ASSERT(ata_sync(0, 0));

    char back[512];
    k_memset(back, 0, sizeof back);
    KTEST_ASSERT_EQ((int)fs_read_range(SCRATCH, 0, back, sizeof back), (int)sizeof back);
    KTEST_ASSERT_EQ(k_memcmp(back, buf, sizeof buf), 0);

    fs_delete(SCRATCH);
}

KTEST("atac", "data larger than the cache bypasses it and stays correct") {
    // The bypass path is where a write-back cache goes wrong quietly:
    // a large transfer skips the cache, so any overlapping cached line
    // has to be reconciled or the two disagree. 64 KiB is comfortably
    // past ATAC_MAX_LINES sectors, so this crosses the boundary in both
    // directions -- which is the point, and is exactly the kind of
    // fixture this project has been caught sizing too small before.
    if (!ata_present()) KTEST_SKIP("no drive attached");

    enum { N = 64 * 1024 };
    static char big[N];
    for (int i = 0; i < N; i++) big[i] = (char)(i ^ (i >> 8));

    fs_delete(SCRATCH);
    KTEST_ASSERT(fs_write_range(SCRATCH, 0, big, N) == 1);
    KTEST_ASSERT(ata_sync(0, 0));

    static char back[N];
    k_memset(back, 0, sizeof back);
    KTEST_ASSERT_EQ((int)fs_read_range(SCRATCH, 0, back, N), N);
    KTEST_ASSERT_EQ(k_memcmp(back, big, N), 0);

    fs_delete(SCRATCH);
}

KTEST("atac", "a flush writes back everything dirty") {
    if (!ata_present()) KTEST_SKIP("no drive attached");

    char buf[512];
    k_memset(buf, 0x5A, sizeof buf);
    fs_delete(SCRATCH);
    KTEST_ASSERT(fs_write_range(SCRATCH, 0, buf, sizeof buf) == 1);

    KTEST_ASSERT(ata_sync(0, 0));

    // Nothing may be left pending after a SUCCESSFUL sync -- that is
    // the whole claim `sync` and the shutdown flush make.
    uint32_t written = 0, pending = 1;
    KTEST_ASSERT(ata_sync(&written, &pending));
    KTEST_ASSERT_EQ((int)pending, 0);
    // ...and a second sync with nothing dirty writes nothing, rather
    // than rewriting the world every time it is called.
    KTEST_ASSERT_EQ((int)written, 0);

    fs_delete(SCRATCH);
}

KTEST("atac", "a failed write-back is REPORTED, not swallowed") {
    // THE LOAD-BEARING TEST. Everything above still passes if flush
    // returns 1 unconditionally; this is the one that does not, and it
    // is what TFS3's journal barriers rest on -- txn_commit() abandons
    // a transaction rather than overwrite targets when the barrier
    // cannot promise durability.
    if (!ata_present()) KTEST_SKIP("no drive attached");
    if (!ata_cache_active()) KTEST_SKIP("no cache to fail");

    char buf[512];
    k_memset(buf, 0xC3, sizeof buf);
    fs_delete(SCRATCH);
    KTEST_ASSERT(fs_write_range(SCRATCH, 0, buf, sizeof buf) == 1);
    KTEST_ASSERT(ata_sync(0, 0)); // start from a clean cache

    // Dirty a line. THE PRECONDITION IS CHECKED, not assumed: if
    // nothing is actually dirty there is no failure to provoke, and
    // every assertion below would pass no matter what write_back() did.
    // The first version of this test wrapped its assertions in
    // `if (!ok)` instead, and a positive control (making a failed
    // write-back report success) reddened NOTHING -- which is exactly
    // the vacuous-check trap this project keeps paying for. Skipping
    // loudly is the honest answer; passing quietly is not.
    KTEST_ASSERT(fs_write_range(SCRATCH, 0, buf, sizeof buf) == 1);
    if (ata_cache_dirty() == 0)
        KTEST_SKIP("nothing dirty to fail -- the fs flushed it already");

    fault_fail_next_ata_writes(64);
    uint32_t written = 0, pending = 0;
    int ok = ata_sync(&written, &pending);
    fault_fail_next_ata_writes(0);

    // Unconditional, and this is the assertion the whole file exists
    // for: a drive that refused every write must NOT produce a
    // successful flush, and the data must still be accounted for rather
    // than dropped.
    KTEST_ASSERT_EQ(ok, 0);
    KTEST_ASSERT(pending > 0);

    // And the cache must recover once the drive stops refusing --
    // otherwise "keeps the line dirty" would just mean "stuck".
    KTEST_ASSERT(ata_sync(&written, &pending));
    KTEST_ASSERT_EQ((int)pending, 0);

    fs_delete(SCRATCH);
}
