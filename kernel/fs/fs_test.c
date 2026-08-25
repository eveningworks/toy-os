// Filesystem tests. The first used to wrap TFS2's tfs_selftest()
// (triple-indirect addressing), which used to run on every disk-backed
// boot -- writing 64 bytes at a 4.6GB offset each time to re-verify
// something that can only break when TFS2 changes.
//
// The rest are new and are the reason the fault injector exists: every
// "what if the disk says no" path added during the storage work was
// previously reachable only by corrupting a disk image from the host
// (tools/tfs3_writer.py corrupt) and booting against it.
#include "ktest.h"
#include "fault_inject.h"
#include "kapi.h"
#include "tfs3.h" // the caps-declaration test below reads tfs3_ops directly
#include "block.h" // blk_sector_count() -- the geometry test at the bottom


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

// NOTE: the triple-indirect addressing test that used to live here was
// TFS2's own selftest, and it went with TFS2. TFS3 has the same three
// indirect levels and the deepest thing exercised below is
// SINGLE-indirect ("truncate cuts a file that uses indirect blocks",
// 20 blocks). That is a real coverage gap and it is on docs/roadmap.md
// rather than pretended away -- a file large enough to reach the
// double- and triple-indirect tables is tens of megabytes, which is
// why it wants its own tool rather than a KTEST.

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
    // The capability assertions used to be conditional on the name,
    // because TFS2 declared none. With TFS2 gone the only backend that
    // can be active on a disk is TFS3 -- and a backend that reported
    // NO capabilities would now be a bug rather than a second format.
    if (fs_is_persistent()) {
        KTEST_ASSERT_EQ(k_strcmp(fs_backend_name(), "tfs3"), 0);
        KTEST_ASSERT(fs_has(FS_CAP_INODES));
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

// ---- rename / truncate ----

KTEST("fs", "rename moves a file, content and identity intact") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_mv_a");
    FRESH("/.ktest_mv_b");
    KTEST_ASSERT(fs_write("/.ktest_mv_a", "payload", 0) == 1);
    struct fs_stat_info before;
    KTEST_ASSERT(fs_stat("/.ktest_mv_a", &before) == 1);

    KTEST_ASSERT(fs_rename("/.ktest_mv_a", "/.ktest_mv_b") == 1);
    // Both halves matter: the old name must be GONE, not merely the
    // new one present -- a rename implemented as a copy passes the
    // second check alone.
    KTEST_ASSERT_EQ(fs_exists("/.ktest_mv_a"), 0);
    KTEST_ASSERT(fs_exists("/.ktest_mv_b") == 1);

    uint32_t size = 0;
    const char *data = fs_read("/.ktest_mv_b", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ(size, 7);
    KTEST_ASSERT(k_strcmp(data, "payload") == 0);

    // Same entry, not a new one -- on tfs3 that is a real inode number,
    // so a copy-and-delete implementation would show a different ino.
    struct fs_stat_info after;
    KTEST_ASSERT(fs_stat("/.ktest_mv_b", &after) == 1);
    if (fs_has(FS_CAP_INODES)) KTEST_ASSERT(before.ino == after.ino);

    fs_delete("/.ktest_mv_b");
}

KTEST("fs", "rename refuses an existing destination and the root") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_mvx_a");
    FRESH("/.ktest_mvx_b");
    KTEST_ASSERT(fs_write("/.ktest_mvx_a", "one", 0) == 1);
    KTEST_ASSERT(fs_write("/.ktest_mvx_b", "two", 0) == 1);

    KTEST_ASSERT_EQ(fs_rename("/.ktest_mvx_a", "/.ktest_mvx_b"), 0);
    // Refused means UNCHANGED, not half-done: both files still there,
    // and the destination still holds its own content.
    KTEST_ASSERT(fs_exists("/.ktest_mvx_a") == 1);
    uint32_t size = 0;
    const char *data = fs_read("/.ktest_mvx_b", &size);
    KTEST_ASSERT(data != 0 && k_strcmp(data, "two") == 0);

    KTEST_ASSERT_EQ(fs_rename("/", "/.ktest_mvx_c"), 0);
    KTEST_ASSERT_EQ(fs_rename("/.ktest_mvx_a", "/"), 0);
    KTEST_ASSERT_EQ(fs_rename("/.ktest_nonexistent", "/.ktest_mvx_c"), 0);
    // Renaming to itself is a no-op, not a failure.
    KTEST_ASSERT(fs_rename("/.ktest_mvx_a", "/.ktest_mvx_a") == 1);
    KTEST_ASSERT(fs_exists("/.ktest_mvx_a") == 1);

    fs_delete("/.ktest_mvx_a");
    fs_delete("/.ktest_mvx_b");
}

KTEST("fs", "rename moves a directory and its contents between parents") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    fs_delete("/.ktest_md/sub/f");
    fs_delete("/.ktest_md/sub");
    fs_delete("/.ktest_md2/sub/f");
    fs_delete("/.ktest_md2/sub");
    fs_delete("/.ktest_md");
    fs_delete("/.ktest_md2");
    KTEST_ASSERT_EQ(fs_exists("/.ktest_md"), 0);
    KTEST_ASSERT_EQ(fs_exists("/.ktest_md2"), 0);

    KTEST_ASSERT(fs_mkdir("/.ktest_md") == 1);
    KTEST_ASSERT(fs_mkdir("/.ktest_md2") == 1);
    KTEST_ASSERT(fs_mkdir("/.ktest_md/sub") == 1);
    KTEST_ASSERT(fs_write("/.ktest_md/sub/f", "deep", 0) == 1);

    // Into its own subtree: refused, or the subtree becomes a cycle
    // nothing can reach.
    KTEST_ASSERT_EQ(fs_rename("/.ktest_md", "/.ktest_md/sub/inner"), 0);

    // A same-parent directory rename fits every journal, so it is
    // asserted unconditionally.
    KTEST_ASSERT(fs_rename("/.ktest_md/sub", "/.ktest_md/moved") == 1);
    KTEST_ASSERT(fs_is_dir("/.ktest_md/moved") == 1);
    KTEST_ASSERT_EQ(fs_exists("/.ktest_md/sub"), 0);
    KTEST_ASSERT(fs_rename("/.ktest_md/moved", "/.ktest_md/sub") == 1);

    // Changing PARENTS is the five-block case a TFS3 v1 image's
    // four-slot journal genuinely cannot express (see tfs3.c's
    // geometry comment). Refusing is the correct answer there, so
    // check the refusal changed nothing and skip the rest rather than
    // reporting a failure against an older on-disk format.
    if (!fs_rename("/.ktest_md/sub", "/.ktest_md2/sub")) {
        KTEST_ASSERT(fs_is_dir("/.ktest_md/sub") == 1);
        KTEST_ASSERT_EQ(fs_exists("/.ktest_md2/sub"), 0);
        fs_delete("/.ktest_md/sub/f");
        fs_delete("/.ktest_md/sub");
        fs_delete("/.ktest_md");
        fs_delete("/.ktest_md2");
        KTEST_SKIP("journal too small for a cross-parent directory move (tfs3 v1 image)");
    }
    KTEST_ASSERT_EQ(fs_exists("/.ktest_md/sub"), 0);
    KTEST_ASSERT(fs_is_dir("/.ktest_md2/sub") == 1);
    // The descendant moved with it -- the part a rename that only
    // repoints the directory itself gets wrong.
    uint32_t size = 0;
    const char *data = fs_read("/.ktest_md2/sub/f", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT(k_strcmp(data, "deep") == 0);
    // And listing the new parent finds it, which is what proves the
    // ".." fixup and the parent's own directory data agree.
    KTEST_ASSERT(fs_is_dir("/.ktest_md2") == 1);

    fs_delete("/.ktest_md2/sub/f");
    fs_delete("/.ktest_md2/sub");
    fs_delete("/.ktest_md");
    fs_delete("/.ktest_md2");
}

KTEST("fs", "truncate shrinks, frees blocks, and grows sparsely") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_trunc");

    static char chunk[16384];
    for (int i = 0; i < 16384; i++) chunk[i] = (char)('A' + (i % 26));

    uint64_t used_empty = 0, total = 0;
    fs_disk_usage(&used_empty, &total);
    KTEST_ASSERT(fs_write_range("/.ktest_trunc", 0, chunk, sizeof(chunk)) == 1);
    KTEST_ASSERT_EQ((int64_t)fs_size("/.ktest_trunc"), 16384);

    uint64_t used_full = 0;
    fs_disk_usage(&used_full, &total);
    KTEST_ASSERT(used_full > used_empty);

    // Shrink to a non-block multiple, so the partial final block is
    // kept and only the whole blocks past it are freed.
    KTEST_ASSERT(fs_truncate("/.ktest_trunc", 5000) == 1);
    KTEST_ASSERT_EQ((int64_t)fs_size("/.ktest_trunc"), 5000);
    uint64_t used_small = 0;
    fs_disk_usage(&used_small, &total);
    KTEST_ASSERT(used_small < used_full); // blocks actually came back

    // The surviving prefix is byte-for-byte what was written -- a
    // truncate that dropped the wrong blocks still reports the right
    // size.
    static char back[5000];
    KTEST_ASSERT_EQ(fs_read_range("/.ktest_trunc", 0, back, sizeof(back)), 5000u);
    for (int i = 0; i < 5000; i++) KTEST_ASSERT(back[i] == chunk[i]);
    // ...and reading past the new end returns nothing.
    KTEST_ASSERT_EQ(fs_read_range("/.ktest_trunc", 5000, back, 16), 0u);

    // Grow: the new range reads as zeros and costs no blocks.
    uint64_t before_grow = 0;
    fs_disk_usage(&before_grow, &total);
    KTEST_ASSERT(fs_truncate("/.ktest_trunc", 40000) == 1);
    KTEST_ASSERT_EQ((int64_t)fs_size("/.ktest_trunc"), 40000);
    uint64_t after_grow = 0;
    fs_disk_usage(&after_grow, &total);
    KTEST_ASSERT_EQ((int64_t)after_grow, (int64_t)before_grow);

    static char zeros[512];
    KTEST_ASSERT_EQ(fs_read_range("/.ktest_trunc", 20000, zeros, sizeof(zeros)), 512u);
    for (int i = 0; i < 512; i++) KTEST_ASSERT_EQ(zeros[i], 0);

    // To zero and back to the starting usage -- the same
    // exact-reclaim assertion the delete test makes.
    KTEST_ASSERT(fs_truncate("/.ktest_trunc", 0) == 1);
    KTEST_ASSERT_EQ((int64_t)fs_size("/.ktest_trunc"), 0);
    uint64_t used_zero = 0;
    fs_disk_usage(&used_zero, &total);
    KTEST_ASSERT_EQ((int64_t)used_zero, (int64_t)used_empty);

    fs_delete("/.ktest_trunc");
}

KTEST("fs", "truncate cuts a file that uses indirect blocks") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_tind");

    // 20 blocks: past the 12 direct pointers, so the single-indirect
    // table is in play. Cutting to 15 blocks leaves that table
    // STRADDLING the cut -- partly kept, partly dropped -- which is
    // the only case the two-phase truncate has to keep an original
    // table image around for, and the case a file that fits in the
    // direct pointers never reaches. (The first version of these
    // tests wrote 4 blocks and a positive control that disabled the
    // boundary handling entirely turned nothing red.)
    enum { BLK = 4096, NBLK = 20, KEEP = 15 };
    static char chunk[BLK];
    uint64_t used_before = 0, total = 0;
    fs_disk_usage(&used_before, &total);
    for (int b = 0; b < NBLK; b++) {
        for (int i = 0; i < BLK; i++) chunk[i] = (char)('a' + ((b + i) % 26));
        KTEST_ASSERT(fs_write_range("/.ktest_tind", (uint64_t)b * BLK, chunk, BLK) == 1);
    }
    KTEST_ASSERT_EQ((int64_t)fs_size("/.ktest_tind"), (int64_t)NBLK * BLK);
    uint64_t used_full = 0;
    fs_disk_usage(&used_full, &total);

    KTEST_ASSERT(fs_truncate("/.ktest_tind", (uint64_t)KEEP * BLK) == 1);
    KTEST_ASSERT_EQ((int64_t)fs_size("/.ktest_tind"), (int64_t)KEEP * BLK);

    // The dropped blocks are actually back -- not merely unreferenced.
    uint64_t used_cut = 0;
    fs_disk_usage(&used_cut, &total);
    KTEST_ASSERT((int64_t)(used_full - used_cut) >= (int64_t)(NBLK - KEEP) * BLK);

    // The kept blocks past the direct pointers still read correctly,
    // which is what proves the straddling table was rewritten rather
    // than dropped.
    static char back[BLK];
    for (int b = 12; b < KEEP; b++) {
        KTEST_ASSERT_EQ(fs_read_range("/.ktest_tind", (uint64_t)b * BLK, back, BLK), (uint32_t)BLK);
        for (int i = 0; i < BLK; i++) KTEST_ASSERT(back[i] == (char)('a' + ((b + i) % 26)));
    }

    // And the bookkeeping is intact: a boundary table left pointing at
    // freed blocks shows up here and nowhere else.
    struct fs_check_result r;
    KTEST_ASSERT(fs_check(0, &r) == 1);
    KTEST_ASSERT_EQ(r.leaked, 0);
    KTEST_ASSERT_EQ(r.double_allocated, 0);
    KTEST_ASSERT_EQ(r.referenced_but_free, 0);
    KTEST_ASSERT_EQ(r.out_of_range, 0);

    KTEST_ASSERT(fs_delete("/.ktest_tind") == 1);
    // Deleting the truncated file returns everything -- a leak in the
    // truncate shows up as a permanent shortfall here.
    uint64_t used_after = 0;
    fs_disk_usage(&used_after, &total);
    KTEST_ASSERT_EQ((int64_t)used_after, (int64_t)used_before);
}

KTEST("fs", "truncate refuses a directory and no-ops at the same size") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    fs_delete("/.ktest_td");
    KTEST_ASSERT(fs_mkdir("/.ktest_td") == 1);
    KTEST_ASSERT_EQ(fs_truncate("/.ktest_td", 0), 0);
    KTEST_ASSERT(fs_is_dir("/.ktest_td") == 1);
    fs_delete("/.ktest_td");

    FRESH("/.ktest_tn");
    KTEST_ASSERT(fs_write("/.ktest_tn", "1234", 0) == 1);
    KTEST_ASSERT(fs_truncate("/.ktest_tn", 4) == 1);
    uint32_t size = 0;
    const char *data = fs_read("/.ktest_tn", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ(size, 4);
    KTEST_ASSERT(k_strcmp(data, "1234") == 0);
    fs_delete("/.ktest_tn");
}

KTEST("fs", "fsck stays clean across a rename and a truncate") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_fsck_a");
    FRESH("/.ktest_fsck_b");
    static char chunk[12288];
    for (int i = 0; i < 12288; i++) chunk[i] = 'z';
    KTEST_ASSERT(fs_write_range("/.ktest_fsck_a", 0, chunk, sizeof(chunk)) == 1);
    KTEST_ASSERT(fs_truncate("/.ktest_fsck_a", 3000) == 1);
    KTEST_ASSERT(fs_rename("/.ktest_fsck_a", "/.ktest_fsck_b") == 1);

    // The point of this one: a partial truncate rewrites a pointer
    // table, and a rename rewrites dirents. Either getting the
    // bookkeeping wrong shows up here as a leak, a double allocation,
    // or a pointer into freed space -- none of which the functional
    // assertions above can see.
    struct fs_check_result r;
    KTEST_ASSERT(fs_check(0, &r) == 1);
    KTEST_ASSERT_EQ(r.leaked, 0);
    KTEST_ASSERT_EQ(r.double_allocated, 0);
    KTEST_ASSERT_EQ(r.referenced_but_free, 0);
    KTEST_ASSERT_EQ(r.out_of_range, 0);

    fs_delete("/.ktest_fsck_b");
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
    fault_fail_next_block_writes(64);
    int created = fs_touch("/.ktest_meta");
    fault_fail_next_block_writes(0);

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
    fault_fail_next_block_writes(64);
    int ok = fs_write_range("/.ktest_dwrite", 0, chunk, sizeof(chunk));
    fault_fail_next_block_writes(0);
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
    fault_fail_next_block_reads(64);
    uint32_t got = fs_read_range("/.ktest_dread", 0, readback, sizeof(readback));
    fault_fail_next_block_reads(0);
    // A read that can't reach the disk must come back short (or zero),
    // never claim a full-length read of whatever was in the buffer.
    KTEST_ASSERT(got < sizeof(readback));

    fs_delete("/.ktest_dread");
}

KTEST("fs", "a failed rename leaves both names as they were") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_mvf_a");
    FRESH("/.ktest_mvf_b");
    KTEST_ASSERT(fs_write("/.ktest_mvf_a", "keepme", 0) == 1);

    // A rename that can't reach the disk must report failure AND leave
    // the namespace exactly as it found it -- the source still there,
    // the destination still absent. Half a rename is the failure this
    // is guarding against, and it is invisible to any test that only
    // checks the return value.
    fault_fail_next_block_writes(64);
    int ok = fs_rename("/.ktest_mvf_a", "/.ktest_mvf_b");
    fault_fail_next_block_writes(0);
    KTEST_ASSERT_EQ(ok, 0);
    KTEST_ASSERT(fs_exists("/.ktest_mvf_a") == 1);
    KTEST_ASSERT_EQ(fs_exists("/.ktest_mvf_b"), 0);

    uint32_t size = 0;
    const char *data = fs_read("/.ktest_mvf_a", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT(k_strcmp(data, "keepme") == 0);

    fs_delete("/.ktest_mvf_a");
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


// A PARTIAL LAST BLOCK GROUP is legal, and the numbers have to agree.
//
// TFS3 used to require whole 128 MiB groups, which made the smallest
// possible filesystem 128 MiB and the live ISO carrying one ten times
// bigger than its contents. ext2/3/4 have always allowed the last group
// to be short; this checks the arithmetic that came with allowing it,
// on whatever volume this boot happens to have.
//
// The trap it guards: `df` reported a 16 MiB volume as 127 MB, because
// the free-space accounting still assumed every group was T3_BPG blocks.
// A size that lies is worse than no size at all.
//
// **This test CANNOT catch that one on its own, and the positive control
// proved it**: re-introduce the bug and this stays green, because on the
// 9 GB dev disk over-reporting by one group is 1.4% of the total and no
// honest bound here is that tight. What catches it is
// tools/live_boot_test.py, which boots a ~24 MB volume -- the only small
// one anything mounts -- and checks df's total against it.
//
// Kept anyway, for the two things it does cover cheaply: a total larger
// than the device, and used larger than total. Recorded rather than
// quietly trusted, because a check whose limits are not written down is
// one somebody later assumes covers more than it does.
KTEST("fs", "reported size matches the volume, partial last group included") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no volume to measure");

    uint64_t total = 0, used = 0;
    if (!fs_disk_usage(&used, &total)) KTEST_SKIP("backend reports no usage");

    // The usable total must fit inside the device, and must not be
    // absurdly smaller than it either -- the old floor-division wasted
    // up to a whole group, and the old T3_BPG assumption over-reported
    // by one. Both directions are checked because they failed in
    // opposite directions.
    uint64_t device = (uint64_t)blk_sector_count() * 512;
    KTEST_ASSERT(total <= device);
    KTEST_ASSERT(used <= total);
    if (device > 64u * 1024 * 1024) {
        // Metadata is a few percent; anything under half the device
        // means a group's worth was dropped on the floor.
        KTEST_ASSERT(total > device / 2);
    }
}

// --- append must PRESERVE what is already in the block ----------------
//
// do_write_inner()'s partial-block path decides whether to read the
// block before modifying it. It used to ask whether the WRITE OFFSET was
// at or past end-of-file -- which an append always is, by definition --
// so every append zeroed its whole block and destroyed the bytes already
// there. `write f AAAA` then `append f BBBB` left four NULs and BBBB.
//
// The fixture has to be SMALLER than a block for this to be reachable at
// all: an append that happens to land on a block boundary takes the
// fresh-block path and is correct either way, so a test written with
// block-aligned data would pass against the bug. That is this repo's
// recurring "the data never crossed the branch" trap, so both sizes are
// here and the small one is the load-bearing half.

KTEST("fs", "append preserves the bytes already in the block") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_append");

    KTEST_ASSERT(fs_write("/.ktest_append", "AAAA", 0) == 1);
    KTEST_ASSERT(fs_write("/.ktest_append", "BBBB", 1) == 1);

    uint32_t size = 0;
    const char *data = fs_read("/.ktest_append", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ(size, 8);
    // The whole point: the HEAD survived. A size check alone passes
    // against the bug -- the file was the right length and full of NULs.
    KTEST_ASSERT(k_strcmp(data, "AAAABBBB") == 0);

    fs_delete("/.ktest_append");
}

KTEST("fs", "repeated appends build one continuous file") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_append2");

    // Four appends, none block-aligned, so every one of them lands mid
    // block and takes the read-modify-write path.
    KTEST_ASSERT(fs_write("/.ktest_append2", "one\n", 0) == 1);
    KTEST_ASSERT(fs_write("/.ktest_append2", "two\n", 1) == 1);
    KTEST_ASSERT(fs_write("/.ktest_append2", "three\n", 1) == 1);
    KTEST_ASSERT(fs_write("/.ktest_append2", "four\n", 1) == 1);

    uint32_t size = 0;
    const char *data = fs_read("/.ktest_append2", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ(size, 19);
    KTEST_ASSERT(k_strcmp(data, "one\ntwo\nthree\nfour\n") == 0);

    fs_delete("/.ktest_append2");
}

KTEST("fs", "an append that crosses a block boundary keeps both halves") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    FRESH("/.ktest_append3");

    // Straddle the boundary deliberately: the tail of block 0 is a
    // read-modify-write, the head of block 1 is a fresh block. A bug in
    // either half shows up as a hole in the middle of the file.
    static char head[4090];
    for (int i = 0; i < (int)sizeof(head) - 1; i++) head[i] = 'x';
    head[sizeof(head) - 1] = '\0';

    KTEST_ASSERT(fs_write("/.ktest_append3", head, 0) == 1);
    KTEST_ASSERT(fs_write("/.ktest_append3", "TAIL", 1) == 1);

    uint32_t size = 0;
    const char *data = fs_read("/.ktest_append3", &size);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ(size, sizeof(head) - 1 + 4);
    KTEST_ASSERT(data[0] == 'x');
    KTEST_ASSERT(data[sizeof(head) - 2] == 'x');
    KTEST_ASSERT(k_strcmp(data + sizeof(head) - 1, "TAIL") == 0);

    fs_delete("/.ktest_append3");
}
