// Tests for the FAT32 backend (kernel/fs/fat32.c).
//
// THEY DRIVE THE BACKEND DIRECTLY, over a RAM volume of their own, not
// through fs_*. The suite runs inside a live kernel with a real root
// mounted and (usually) the real ESP mounted at /boot, so touching the
// active mount would swap it out from under everything else running --
// the same hazard ramfs_test.c and partition_test.c have, and the same
// answer: give the component under test its own device, and put the
// mount back afterwards.
//
// THE VOLUME IS 512 KiB, which is deliberately SMALL and deliberately
// not a round number of clusters. It is big enough for a file that
// spans hundreds of clusters and a directory that outgrows its first
// one -- which are the two places FAT's arithmetic is either right or
// silently truncating -- and small enough that kmalloc()'s contiguous
// allocation succeeds on a fragmented machine.
//
// FAT32 BELOW 65525 CLUSTERS IS OUT OF SPEC for a generic driver, and
// this volume is (976 clusters). That threshold exists to tell FAT12,
// FAT16 and FAT32 apart in a driver implementing all three; this one
// implements only FAT32 and discriminates on the BPB's FAT32-only
// fields instead -- see parse_bpb(). Said here because the number will
// look wrong to anyone who knows the spec.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, and therefore what these
// avoid: a single short file in the root directory works on almost any
// half-written FAT driver. The checks that discriminate are the LONG
// NAME (an LFN set written in the wrong order reads back reversed, and
// this driver shipped that for one build), the MULTI-CLUSTER pattern
// (address-derived, so two clusters aliasing to one is visible rather
// than reading back as the constant that was written), the DIRECTORY
// THAT OUTGROWS A CLUSTER, and PROBE-WHILE-MOUNTED.
#include "ktest.h"
#include "fat32.h"
#include "fs.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "kfmt.h"   // k_snprintf
#include "mount.h"  // restore() puts the machine's own /boot back

#define TEST_SECTORS 1024u          // 512 KiB
#define TEST_BYTES   (TEST_SECTORS * 512u)

static uint8_t *g_img;

static uint32_t img_sector_count(void) { return TEST_SECTORS; }

static int img_read(uint32_t lba, int count, void *buf) {
    if (!g_img || count <= 0) return 0;
    if ((uint64_t)lba + (uint64_t)count > TEST_SECTORS) return 0;
    k_memcpy(buf, g_img + (uint64_t)lba * 512, (uint32_t)count * 512);
    return 1;
}

static int img_write(uint32_t lba, int count, const void *buf) {
    if (!g_img || count <= 0) return 0;
    if ((uint64_t)lba + (uint64_t)count > TEST_SECTORS) return 0;
    k_memcpy(g_img + (uint64_t)lba * 512, buf, (uint32_t)count * 512);
    return 1;
}

static int img_max_xfer(void) { return 8; }

// NOT persistent and NO capabilities: a RAM device has nothing that can
// be lost independently of everything else, so claiming FLUSH would be
// claiming a barrier that means nothing. block.h's honesty rule.
static const struct block_device IMG_DEV = {
    .name = "fattest",
    .sector_count = img_sector_count,
    .read_sectors = img_read,
    .write_sectors = img_write,
    .max_sectors_per_xfer = img_max_xfer,
    .persistent = 0,
    .caps = 0,
    .flush = 0,
    .trim = 0,
};

static const struct fs_ops *F(void) { return &fat32_ops; }

// A fresh, formatted, mounted volume. Returns 0 if the image could not
// be allocated, which is a SKIP rather than a failure -- a fragmented
// heap is not a bug in this filesystem.
static int fresh(void) {
    if (!g_img) g_img = kmalloc(TEST_BYTES);
    if (!g_img) return 0;
    k_memset(g_img, 0, TEST_BYTES);
    if (!F()->format(&IMG_DEV)) return 0;
    return F()->init(&IMG_DEV) == 1;
}

// Puts the machine's own filesystem back. The backend is single-mount
// (fs_ops.max_mounts), so a test that leaves it pointed at the RAM
// image leaves the REAL /boot broken -- which would surface much later,
// in some other tool, as a filesystem that is suddenly empty.
static void restore(void) {
    if (F()->umount) F()->umount(&IMG_DEV);
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (m && m->fs == &fat32_ops) {
            F()->init(m->dev);
            return;
        }
    }
}

KTEST("fat32", "formats a volume, mounts it, and the root is an empty directory") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    KTEST_ASSERT(F()->is_dir("/"));
    KTEST_ASSERT(F()->exists("/"));
    KTEST_ASSERT(!F()->exists("/nothing"));

    uint64_t used = 1, total = 0;
    KTEST_ASSERT(F()->disk_usage(&used, &total));
    // Exactly one cluster is in use on a fresh volume: the root
    // directory's. A driver that counted the reserved entries as data
    // (or forgot the root) reports 0 or 2 here.
    KTEST_ASSERT_EQ((int)(used / 512), 1);
    KTEST_ASSERT(total > 400 * 1024);
    restore();
}

KTEST("fat32", "a probe recognises what format() wrote, and refuses a blank volume") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    KTEST_ASSERT_EQ(F()->probe(&IMG_DEV), 1);
    // A blank volume is "readable, but not mine" -- 0, never -1, which
    // vfs.c reads as "could not read at all" and refuses to touch.
    k_memset(g_img, 0, TEST_BYTES);
    KTEST_ASSERT_EQ(F()->probe(&IMG_DEV), 0);
    // And wipe() must leave it in that state from a formatted one.
    KTEST_ASSERT(F()->format(&IMG_DEV));
    KTEST_ASSERT_EQ(F()->probe(&IMG_DEV), 1);
    KTEST_ASSERT(F()->wipe(&IMG_DEV));
    KTEST_ASSERT_EQ(F()->probe(&IMG_DEV), 0);
    restore();
}

KTEST("fat32", "a short-named file is written, read back and deleted") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    KTEST_ASSERT(F()->write("/hello.txt", "fat32", 0));
    KTEST_ASSERT(F()->exists("/hello.txt"));
    KTEST_ASSERT(!F()->is_dir("/hello.txt"));
    KTEST_ASSERT_EQ((int)F()->size("/hello.txt"), 5);

    char buf[16];
    k_memset(buf, 0, sizeof buf);
    KTEST_ASSERT_EQ((int)F()->read_range("/hello.txt", 0, buf, sizeof buf), 5);
    KTEST_ASSERT_EQ(k_strcmp(buf, "fat32"), 0);

    KTEST_ASSERT(F()->del("/hello.txt"));
    KTEST_ASSERT(!F()->exists("/hello.txt"));
    restore();
}

// THE ONE A BROKEN LFN WRITER FAILS. The set is stored highest-index
// first on disk; writing it the other way round produces a name that
// reads back in 13-character blocks in reverse order -- which this
// driver did on its first build, and which a short name cannot show.
KTEST("fat32", "a long file name round-trips, and is not its 8.3 alias") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    const char *lname = "/a_very_long_filename_indeed.txt";
    KTEST_ASSERT(F()->write(lname, "longname", 0));
    KTEST_ASSERT(F()->exists(lname));
    KTEST_ASSERT_EQ((int)F()->size(lname), 8);

    // The alias exists on disk and is NOT the name -- so a lookup that
    // silently fell back to the 8.3 entry would fail this, while a
    // lookup that found the long name passes both.
    KTEST_ASSERT(!F()->exists("/A_VERY~1.TXT"));

    // A SECOND long name sharing the first six characters must get a
    // different alias. Without the ~N collision search both entries
    // claim A_VERY~1.TXT and the second overwrites the first.
    KTEST_ASSERT(F()->write("/a_very_long_filename_second.txt", "two", 0));
    KTEST_ASSERT(F()->exists(lname));
    KTEST_ASSERT(F()->exists("/a_very_long_filename_second.txt"));
    KTEST_ASSERT_EQ((int)F()->size(lname), 8);
    restore();
}

// Multi-cluster, with an ADDRESS-DERIVED pattern: a constant fill
// cannot detect two file offsets mapping to one cluster, because both
// read back the constant and look perfect. /tests/memtest makes the
// same call for the same reason.
KTEST("fat32", "a file spanning hundreds of clusters reads back exactly") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    enum { N = 200 * 1024 };
    uint8_t *src = kmalloc(N);
    uint8_t *dst = kmalloc(N);
    if (!src || !dst) {
        if (src) kfree(src);
        if (dst) kfree(dst);
        restore();
        KTEST_SKIP("could not allocate the 200 KiB pattern buffers");
    }
    for (unsigned i = 0; i < N; i++) src[i] = (uint8_t)((i * 31u) ^ (i >> 11));

    KTEST_ASSERT(F()->touch("/big.bin"));
    KTEST_ASSERT(F()->write_range("/big.bin", 0, src, N));
    KTEST_ASSERT_EQ((int)F()->size("/big.bin"), N);

    k_memset(dst, 0, N);
    uint32_t got = 0;
    while (got < N) {
        uint32_t n = F()->read_range("/big.bin", got, dst + got, N - got);
        if (!n) break;
        got += n;
    }
    KTEST_ASSERT_EQ((int)got, N);
    KTEST_ASSERT_EQ(k_memcmp(src, dst, N), 0);

    // And deleting it gives the clusters back -- the free count is what
    // a leak would show up in.
    uint64_t used_before = 0, total = 0;
    F()->disk_usage(&used_before, &total);
    KTEST_ASSERT(F()->del("/big.bin"));
    uint64_t used_after = 0;
    F()->disk_usage(&used_after, &total);
    KTEST_ASSERT(used_after < used_before);
    KTEST_ASSERT_EQ((int)(used_after / 512), 1); // just the root again

    kfree(src);
    kfree(dst);
    restore();
}

KTEST("fat32", "truncate grows with zeroes and shrinks, and a hole is not stale data") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    // Leave a recognisable pattern on disk first, then delete it, so a
    // grown file landing on those clusters would read back somebody
    // else's bytes if the grow path did not zero them.
    uint8_t *junk = kmalloc(64 * 1024);
    if (!junk) { restore(); KTEST_SKIP("could not allocate the junk buffer"); }
    k_memset(junk, 0xAB, 64 * 1024);
    KTEST_ASSERT(F()->touch("/junk.bin"));
    KTEST_ASSERT(F()->write_range("/junk.bin", 0, junk, 64 * 1024));
    KTEST_ASSERT(F()->del("/junk.bin"));
    kfree(junk);

    KTEST_ASSERT(F()->write("/grow.txt", "abc", 0));
    KTEST_ASSERT(F()->truncate("/grow.txt", 40000));
    KTEST_ASSERT_EQ((int)F()->size("/grow.txt"), 40000);

    uint8_t probe[512];
    k_memset(probe, 0xFF, sizeof probe);
    KTEST_ASSERT_EQ((int)F()->read_range("/grow.txt", 30000, probe, sizeof probe), 512);
    for (unsigned i = 0; i < sizeof probe; i++) KTEST_ASSERT_EQ(probe[i], 0);

    // The first three bytes survive the grow.
    k_memset(probe, 0, 8);
    KTEST_ASSERT_EQ((int)F()->read_range("/grow.txt", 0, probe, 3), 3);
    KTEST_ASSERT_EQ(probe[0], 'a');

    KTEST_ASSERT(F()->truncate("/grow.txt", 3));
    KTEST_ASSERT_EQ((int)F()->size("/grow.txt"), 3);
    // Reading past the new end is EOF, not the bytes that are still
    // physically in the cluster.
    KTEST_ASSERT_EQ((int)F()->read_range("/grow.txt", 3, probe, 16), 0);

    KTEST_ASSERT(F()->truncate("/grow.txt", 0));
    KTEST_ASSERT_EQ((int)F()->size("/grow.txt"), 0);
    restore();
}

KTEST("fat32", "directories nest, refuse deletion while occupied, and survive a rename") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    KTEST_ASSERT(F()->mkdir("/a"));
    KTEST_ASSERT(F()->is_dir("/a"));
    KTEST_ASSERT(F()->mkdir("/a/b"));
    KTEST_ASSERT(F()->write("/a/b/leaf.txt", "deep", 0));
    KTEST_ASSERT_EQ((int)F()->size("/a/b/leaf.txt"), 4);

    // A non-empty directory is refused, which is fs.h's contract.
    KTEST_ASSERT(!F()->del("/a"));
    KTEST_ASSERT(!F()->del("/a/b"));

    // Moving a directory has to fix its `..`, or its children point at
    // a parent it no longer lives in. The check that sees it: the
    // subtree is still reachable by its new path.
    KTEST_ASSERT(F()->mkdir("/c"));
    KTEST_ASSERT(F()->rename("/a/b", "/c/moved"));
    KTEST_ASSERT(F()->is_dir("/c/moved"));
    KTEST_ASSERT(!F()->exists("/a/b"));
    KTEST_ASSERT_EQ((int)F()->size("/c/moved/leaf.txt"), 4);

    KTEST_ASSERT(F()->del("/a"));     // empty now
    restore();
}

// A directory OUTGROWING ITS FIRST CLUSTER. At 512-byte clusters that
// is 16 entries, and a long name costs four of them -- so twenty files
// is several clusters' worth. A driver that never extends a directory
// fails at the sixteenth entry; one that extends it but loses the chain
// fails on the read back.
KTEST("fat32", "a directory grows past one cluster and every entry survives") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    KTEST_ASSERT(F()->mkdir("/many"));
    char path[64];
    for (int i = 0; i < 20; i++) {
        k_snprintf(path, sizeof path, "/many/entry_number_%d_of_twenty.txt", i);
        KTEST_ASSERT(F()->write(path, "x", 0));
    }
    for (int i = 0; i < 20; i++) {
        k_snprintf(path, sizeof path, "/many/entry_number_%d_of_twenty.txt", i);
        KTEST_ASSERT(F()->exists(path));
        KTEST_ASSERT_EQ((int)F()->size(path), 1);
    }
    // And they all go away again, leaving the directory deletable.
    for (int i = 0; i < 20; i++) {
        k_snprintf(path, sizeof path, "/many/entry_number_%d_of_twenty.txt", i);
        KTEST_ASSERT(F()->del(path));
    }
    KTEST_ASSERT(F()->del("/many"));
    restore();
}

// THE BUG THIS DRIVER SHIPPED FOR ONE BOOT, in its own words: a probe
// is documented as side-effect-free, and both backends here record the
// device they were handed. Mounting /boot probes every backend against
// the ESP, so a TFS3 already serving `/` was repointed at partition 2
// and the root went silently empty. This asserts the FAT32 half of the
// same rule, which is the half a test can drive.
KTEST("fat32", "probing a second volume does not disturb the mounted one") {
    if (!fresh()) { KTEST_SKIP("could not allocate a 512 KiB test volume"); }
    KTEST_ASSERT(F()->write("/mounted.txt", "still here", 0));

    // The real disk's own device is the honest second volume: whatever
    // is active, it is not this RAM image.
    const struct block_device *elsewhere = blk_active();
    if (elsewhere && elsewhere != &IMG_DEV) F()->probe(elsewhere);

    // The mount is untouched.
    KTEST_ASSERT(F()->exists("/mounted.txt"));
    KTEST_ASSERT_EQ((int)F()->size("/mounted.txt"), 10);
    restore();
}
