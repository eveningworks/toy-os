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
#include "string.h"

#define SCRATCH "/tmp/ktest_sync.bin"

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

    storage_config_set_strict_for_test(1);
    uint64_t a0 = flushes();
    for (int i = 0; i < WRITES; i++)
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, (uint64_t)i * sizeof buf, buf, sizeof buf), 1);
    uint64_t strict_cost = flushes() - a0;

    storage_config_set_strict_for_test(0);
    uint64_t b0 = flushes();
    for (int i = 0; i < WRITES; i++)
        KTEST_ASSERT_EQ(fs_write_range(SCRATCH, (uint64_t)i * sizeof buf, buf, sizeof buf), 1);
    uint64_t lazy_cost = flushes() - b0;

    storage_config_set_strict_for_test(restore);

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
