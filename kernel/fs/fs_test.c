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

#define TEST_PATH "/.ktest_tmp"

KTEST("fs", "triple-indirect addressing (legacy selftest)") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    KTEST_ASSERT(tfs_selftest() == 1);
}

KTEST("fs", "write then read back") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    fs_delete(TEST_PATH);
    KTEST_ASSERT(fs_write(TEST_PATH, "hello ktest", 0) == 1);
    uint32_t size = 0;
    const char *data = fs_read(TEST_PATH, &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ(size, 11);
    KTEST_ASSERT(k_strcmp(data, "hello ktest") == 0);
    fs_delete(TEST_PATH);
}

KTEST("fs", "delete frees the file's blocks") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    uint64_t used_before = 0, total = 0;
    fs_disk_usage(&used_before, &total);

    static char chunk[8192];
    for (int i = 0; i < 8192; i++) chunk[i] = (char)('a' + (i % 26));
    KTEST_ASSERT(fs_write_range(TEST_PATH, 0, chunk, sizeof(chunk)) == 1);

    uint64_t used_during = 0;
    fs_disk_usage(&used_during, &total);
    KTEST_ASSERT(used_during > used_before);

    KTEST_ASSERT(fs_delete(TEST_PATH) == 1);
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
    fs_delete(TEST_PATH);

    // persist_record() writes the journal data, the commit header and
    // the table slot. Fail all of them: fs_touch() must report failure
    // rather than returning success for a file that isn't on disk.
    fault_fail_next_ata_writes(64);
    int created = fs_touch(TEST_PATH);
    fault_fail_next_ata_writes(0);

    KTEST_ASSERT_EQ(created, 0);
    // And the in-memory table must agree -- the rollback in tfs_touch()
    // is what stops a file existing until the next reboot and then not.
    KTEST_ASSERT_EQ(fs_exists(TEST_PATH), 0);
}

KTEST("fs", "a failed data write is reported") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    fs_delete(TEST_PATH);
    KTEST_ASSERT(fs_touch(TEST_PATH) == 1);

    static char chunk[4096];
    for (int i = 0; i < 4096; i++) chunk[i] = 'x';
    fault_fail_next_ata_writes(64);
    int ok = fs_write_range(TEST_PATH, 0, chunk, sizeof(chunk));
    fault_fail_next_ata_writes(0);
    KTEST_ASSERT_EQ(ok, 0);

    fs_delete(TEST_PATH);
}

KTEST("fs", "a failed read is reported, not silently short") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    fs_delete(TEST_PATH);
    static char chunk[4096];
    for (int i = 0; i < 4096; i++) chunk[i] = 'y';
    KTEST_ASSERT(fs_write_range(TEST_PATH, 0, chunk, sizeof(chunk)) == 1);

    static char readback[4096];
    fault_fail_next_ata_reads(64);
    uint32_t got = fs_read_range(TEST_PATH, 0, readback, sizeof(readback));
    fault_fail_next_ata_reads(0);
    // A read that can't reach the disk must come back short (or zero),
    // never claim a full-length read of whatever was in the buffer.
    KTEST_ASSERT(got < sizeof(readback));

    fs_delete(TEST_PATH);
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
