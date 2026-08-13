// Filesystem tests. The first wraps the pre-existing tfs_selftest()
// (triple-indirect addressing), which used to run on every disk-backed
// boot -- writing 64 bytes at a 4.6GB offset each time to re-verify
// something that can only break when tfs.c changes.
//
// The rest are new and are the reason the fault injector exists: every
// "what if the disk says no" path added during the storage work was
// previously reachable only by corrupting a disk image from the host
// (tools/tfs2_writer.py corrupt) and booting against it.
#include "ktest.h"
#include "fault_inject.h"
#include "kapi.h"

int tfs_selftest(void); // kernel/fs/tfs.c -- needs its file-static state

// One path per test, not one shared path, and every test asserts its
// own precondition rather than assuming it.
//
// CI taught this: a real transient DMA write failed on a contended
// runner, one test's cleanup delete silently didn't happen, and the
// NEXT test then called fs_touch() on a file that already existed.
// tfs_touch() returns success immediately for an existing file without
// writing anything, so the fault injector never fired and the test
// failed reporting a symptom three steps from the cause.
//
// FRESH() below deletes and then asserts the file is really gone, so a
// test that can't establish its own starting state says so.

#define FRESH(path)                                                            \
    do {                                                                       \
        fs_delete(path);                                                       \
        KTEST_ASSERT_EQ(fs_exists(path), 0); /* precondition, not assumption */\
    } while (0)

KTEST("fs", "triple-indirect addressing (legacy selftest)") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    KTEST_ASSERT(tfs_selftest() == 1);
}

KTEST("fs", "write then read back") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_rw");
    KTEST_ASSERT(fs_write("/.ktest_rw", "hello ktest", 0) == 1);
    uint32_t size = 0;
    const char *data = fs_read("/.ktest_rw", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ(size, 11);
    KTEST_ASSERT(k_strcmp(data, "hello ktest") == 0);
    fs_delete("/.ktest_rw");
}

KTEST("fs", "delete frees the file's blocks") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_blocks");
    uint64_t used_before = 0, total = 0;
    fs_disk_usage(&used_before, &total);

    static char chunk[8192];
    for (int i = 0; i < 8192; i++) chunk[i] = (char)('a' + (i % 26));
    KTEST_ASSERT(fs_write_range("/.ktest_blocks", 0, chunk, sizeof(chunk)) == 1);

    uint64_t used_during = 0;
    fs_disk_usage(&used_during, &total);
    KTEST_ASSERT(used_during > used_before);

    KTEST_ASSERT(fs_delete("/.ktest_blocks") == 1);
    uint64_t used_after = 0;
    fs_disk_usage(&used_after, &total);
    // Exactly back to where it started -- this is the ordering the
    // truncate/delete path deliberately gets right (docs/decisions.md:
    // persist the record first, free the blocks second).
    KTEST_ASSERT_EQ((int64_t)used_after, (int64_t)used_before);
}

// ---- error paths, reachable only via fault injection ----

KTEST("fs", "a failed metadata write is reported, not swallowed") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    // MUST be absent: tfs_touch() returns success for an existing file
    // without writing anything, so a leftover file would make this test
    // pass the injector by entirely.
    FRESH("/.ktest_meta");

    // persist_record() writes the journal data, the commit header and
    // the table slot. Fail all of them: fs_touch() must report failure
    // rather than returning success for a file that isn't on disk.
    fault_fail_next_ata_writes(64);
    int created = fs_touch("/.ktest_meta");
    fault_fail_next_ata_writes(0);

    KTEST_ASSERT_EQ(created, 0);
    // And the in-memory table must agree -- the rollback in tfs_touch()
    // is what stops a file existing until the next reboot and then not.
    KTEST_ASSERT_EQ(fs_exists("/.ktest_meta"), 0);
}

KTEST("fs", "a failed data write is reported") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_dwrite");
    KTEST_ASSERT(fs_touch("/.ktest_dwrite") == 1);

    static char chunk[4096];
    for (int i = 0; i < 4096; i++) chunk[i] = 'x';
    fault_fail_next_ata_writes(64);
    int ok = fs_write_range("/.ktest_dwrite", 0, chunk, sizeof(chunk));
    fault_fail_next_ata_writes(0);
    KTEST_ASSERT_EQ(ok, 0);

    fs_delete("/.ktest_dwrite");
}

KTEST("fs", "a failed read is reported, not silently short") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_dread");
    static char chunk[4096];
    for (int i = 0; i < 4096; i++) chunk[i] = 'y';
    KTEST_ASSERT(fs_write_range("/.ktest_dread", 0, chunk, sizeof(chunk)) == 1);

    static char readback[4096];
    fault_fail_next_ata_reads(64);
    uint32_t got = fs_read_range("/.ktest_dread", 0, readback, sizeof(readback));
    fault_fail_next_ata_reads(0);
    // A read that can't reach the disk must come back short (or zero),
    // never claim a full-length read of whatever was in the buffer.
    KTEST_ASSERT(got < sizeof(readback));

    fs_delete("/.ktest_dread");
}

KTEST("fs", "fsck reports a clean filesystem") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    struct fs_check_result r;
    KTEST_ASSERT(fs_check(0, &r) == 1);
    KTEST_ASSERT_EQ(r.leaked, 0);
    KTEST_ASSERT_EQ(r.double_allocated, 0);
    KTEST_ASSERT_EQ(r.referenced_but_free, 0);
    KTEST_ASSERT_EQ(r.out_of_range, 0);
}
