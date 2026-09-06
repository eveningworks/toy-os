// Tests for the block layer's per-operation counters (block_stat.h).
//
// What these have to establish is that the counters MOVE, and move for
// the right op -- a counter that silently stays at zero reports a
// healthy system as an idle one, and a diagnostic nobody can trust is
// worse than none. The interesting assertion is therefore a DELTA
// across known I/O, not an absolute value: this runs in a live kernel
// with a desktop doing its own reads, so nothing here may assume it is
// the only writer.
#include "ktest.h"
#include "tmppath.h"
#include "fs.h"
#include "block.h"
#include "block_stat.h"
#include "fs.h"
#include "string.h"
#include "clocksource.h"

// Built from the configured directory rather than spelled out
// (api/tmppath.h), so moving scratch is a setting rather than a grep.
static const char *scratch_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_blkstat.bin");
    return p;
}
#define SCRATCH scratch_path()

static uint64_t calls_of(int op) {
    uint64_t c = 0;
    blk_stat_get(op, &c, NULL, NULL, NULL);
    return c;
}

// Can this boot's clocksource resolve a single driver call at all? Two
// back-to-back reads differing is the cheapest honest answer; the PIT
// gives the same number twice.
static int clock_resolves_a_call(void) {
    for (int i = 0; i < 8; i++) {
        uint64_t a = clocksource_now_ns();
        uint64_t b = clocksource_now_ns();
        if (b > a) return 1;
    }
    return 0;
}

static uint64_t ns_of(int op) {
    uint64_t n = 0;
    blk_stat_get(op, NULL, NULL, &n, NULL);
    return n;
}

KTEST("blkstat", "an op name is reported for every op, and refused past the end") {
    KTEST_ASSERT(!k_strcmp(blk_stat_op_name(BLK_STAT_READ), "read"));
    KTEST_ASSERT(!k_strcmp(blk_stat_op_name(BLK_STAT_WRITE), "write"));
    KTEST_ASSERT(!k_strcmp(blk_stat_op_name(BLK_STAT_FLUSH), "flush"));
    KTEST_ASSERT(!k_strcmp(blk_stat_op_name(BLK_STAT_TRIM), "trim"));
    KTEST_ASSERT(!k_strcmp(blk_stat_op_name(BLK_STAT_OPS), "?"));
    KTEST_ASSERT(!k_strcmp(blk_stat_op_name(-1), "?"));
}

KTEST("blkstat", "an out-of-range op is refused rather than counted") {
    uint64_t before = calls_of(BLK_STAT_READ);
    blk_stat_add(BLK_STAT_OPS, 8, 1000, 1);
    blk_stat_add(-1, 8, 1000, 1);
    KTEST_ASSERT_EQ(calls_of(BLK_STAT_READ), before);
    KTEST_ASSERT_EQ(blk_stat_get(BLK_STAT_OPS, NULL, NULL, NULL, NULL), 0);
    KTEST_ASSERT_EQ(blk_stat_get(-1, NULL, NULL, NULL, NULL), 0);
}

// THE ONE THAT WOULD CATCH THE COUNTERS BEING DEAD. Writing a file has
// to move the write counter and spend time; if it does not, block.c's
// entry points are not going through the timed helpers at all.
KTEST("blkstat", "writing a file moves the write counters") {
    if (!fs_is_persistent()) { KTEST_SKIP("no persistent filesystem"); return; }

    static uint8_t buf[8192];
    k_memset(buf, 0xA5, sizeof buf);

    KTEST_ASSERT_EQ(fs_touch(SCRATCH), 1);

    uint64_t calls0 = calls_of(BLK_STAT_WRITE);
    uint64_t ns0 = ns_of(BLK_STAT_WRITE);

    KTEST_ASSERT_EQ(fs_write_range(SCRATCH, 0, buf, sizeof buf), 1);

    KTEST_ASSERT((calls_of(BLK_STAT_WRITE)) > (calls0));

    // TIME, not just a count -- a helper that counted and forgot to
    // bracket the call with the clock is the likely way this rots.
    //
    // GATED ON THE CLOCK BEING ABLE TO SEE IT, because in every default
    // QEMU configuration it cannot: an invariant TSC is not offered to
    // the guest unless the CPU model says `+invtsc` (it blocks
    // migration), so the clocksource falls back to the PIT and a single
    // driver call rounds to zero. Asserting anyway would fail on every
    // local run; asserting nothing would let a dead timer through on
    // hardware, where the TSC wins and this is the check that matters.
    if (!clock_resolves_a_call()) {
        KTEST_SKIP("clocksource too coarse to time one call (PIT; needs +invtsc under QEMU)");
    }
    KTEST_ASSERT((ns_of(BLK_STAT_WRITE)) > (ns0));

    fs_delete(SCRATCH);
}

// A flush is the counter this whole class exists for -- it moves no
// sectors, so nothing else in the layer would ever notice it.
KTEST("blkstat", "a flush is counted and moves no sectors") {
    if (!fs_is_persistent()) { KTEST_SKIP("no persistent filesystem"); return; }

    uint64_t calls0 = 0, sectors0 = 0;
    blk_stat_get(BLK_STAT_FLUSH, &calls0, &sectors0, NULL, NULL);

    KTEST_ASSERT_EQ(blk_flush(), 1);

    uint64_t calls1 = 0, sectors1 = 0;
    blk_stat_get(BLK_STAT_FLUSH, &calls1, &sectors1, NULL, NULL);
    KTEST_ASSERT((calls1) > (calls0));
    KTEST_ASSERT_EQ(sectors1, sectors0);
}

// SYNC MUST REACH THE DEVICE, and this is the check that was missing
// when it did not. `sys_sync()` was ATA-only -- with no software sector
// cache it returned "nothing was pending" having asked the drive for
// nothing at all, which is every AHCI and virtio-blk machine. Counting
// FLUSHES rather than inspecting the code is the point: the broken
// version returned success, so only the device counter can tell the
// difference between a sync and a no-op.
KTEST("blkstat", "sync flushes the device even with no software cache") {
    if (!fs_is_persistent()) { KTEST_SKIP("no persistent filesystem"); return; }

    uint64_t before = 0;
    blk_stat_get(BLK_STAT_FLUSH, &before, NULL, NULL, NULL);

    uint32_t wrote = 0;
    KTEST_ASSERT_EQ(fs_sync(&wrote), 1);

    uint64_t after = 0;
    blk_stat_get(BLK_STAT_FLUSH, &after, NULL, NULL, NULL);
    // At least one mounted volume, so at least one flush. Not an exact
    // count: the desktop is running and syncs on its own.
    KTEST_ASSERT(after > before);
}

// `/bin/diskbench` subtracts two snapshots, so a counter that went
// BACKWARDS would hand it a wrapped unsigned delta and a nonsense
// report. Reset is the only thing that may do that, and nothing on the
// I/O path calls it.
KTEST("blkstat", "reset zeroes every op") {
    blk_stat_reset();
    for (int op = 0; op < BLK_STAT_OPS; op++) {
        uint64_t calls = 1, sectors = 1, ns = 1, failures = 1;
        KTEST_ASSERT_EQ(blk_stat_get(op, &calls, &sectors, &ns, &failures), 1);
        KTEST_ASSERT_EQ(calls, 0);
        KTEST_ASSERT_EQ(sectors, 0);
        KTEST_ASSERT_EQ(ns, 0);
        KTEST_ASSERT_EQ(failures, 0);
    }
}
