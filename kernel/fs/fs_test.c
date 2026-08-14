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
#include "tfs3.h" // the caps-declaration test below reads tfs3_ops directly

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

KTEST("fs", "tfs3 declares its format capabilities") {
    // Static declaration check -- runs regardless of which backend is
    // mounted. The live tfs3 read/mount/switch behavior is covered by
    // the vm.py-driven tests (tools/fs_switch_test.py, Stage E).
    KTEST_ASSERT(k_strcmp(tfs3_ops.name, "tfs3") == 0);
    KTEST_ASSERT((tfs3_ops.caps & FS_CAP_INODES) != 0);
    KTEST_ASSERT((tfs3_ops.caps & FS_CAP_EPOCH_TIME) != 0);
    KTEST_ASSERT((tfs3_ops.caps & FS_CAP_HARDLINKS) != 0);
    KTEST_ASSERT((tfs3_ops.caps & FS_CAP_SYMLINKS) != 0);
}

KTEST("fs", "backend reports a name and honest capabilities") {
    // Valid in every boot mode -- RAM-only still has an active backend.
    KTEST_ASSERT(fs_backend_name() != 0);
    KTEST_ASSERT(fs_backend_name()[0] != '\0');
    // tfs2 declares no capabilities (no inodes/hardlinks/symlinks, and
    // its timestamps are stored civil, converted at stat time). When
    // tfs3 lands this assertion becomes conditional on the name.
    if (k_strcmp(fs_backend_name(), "tfs2") == 0) {
        KTEST_ASSERT_EQ(fs_capabilities(), 0u);
        KTEST_ASSERT_EQ(fs_has(FS_CAP_HARDLINKS), 0);
    }
}

KTEST("fs", "stat reports a stable ino and sane epoch timestamps") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_stat_a");
    FRESH("/.ktest_stat_b");
    KTEST_ASSERT(fs_write("/.ktest_stat_a", "a", 0) == 1);
    KTEST_ASSERT(fs_write("/.ktest_stat_b", "b", 0) == 1);

    struct fs_stat_info sa, sb, sa2;
    KTEST_ASSERT(fs_stat("/.ktest_stat_a", &sa) == 1);
    KTEST_ASSERT(fs_stat("/.ktest_stat_b", &sb) == 1);
    KTEST_ASSERT(sa.ino != sb.ino); // identity means distinct entries differ
    KTEST_ASSERT(fs_stat("/.ktest_stat_a", &sa2) == 1);
    KTEST_ASSERT(sa.ino == sa2.ino); // ...and repeat stats agree

    // Epochs are local-derived seconds (fs.h) -- the RTC reports a
    // real current date, so anything from a live boot is well past
    // 2020-01-01 (1577836800) and created <= modified always holds.
    KTEST_ASSERT(sa.created >= 1577836800ull);
    KTEST_ASSERT(sa.created <= sa.modified);

    // Root has no entry of its own -- unchanged contract.
    struct fs_stat_info r;
    KTEST_ASSERT_EQ(fs_stat("/", &r), 0);

    fs_delete("/.ktest_stat_a");
    fs_delete("/.ktest_stat_b");
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
    // Honest skip, not a green lie: tfs3's checker lands in Stage D
    // of the TFS3 plan, and claiming "clean" without walking anything
    // is exactly what fs_check()'s contract forbids. Delete this skip
    // when tfs3_check() is real.
    if (k_strcmp(fs_backend_name(), "tfs3") == 0)
        KTEST_SKIP("tfs3 fsck lands in Stage D");
    struct fs_check_result r;
    KTEST_ASSERT(fs_check(0, &r) == 1);
    KTEST_ASSERT_EQ(r.leaked, 0);
    KTEST_ASSERT_EQ(r.double_allocated, 0);
    KTEST_ASSERT_EQ(r.referenced_but_free, 0);
    KTEST_ASSERT_EQ(r.out_of_range, 0);
}
