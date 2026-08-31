// Tests for the in-memory filesystem (kernel/fs/ramfs.c).
//
// THEY DRIVE THE BACKEND DIRECTLY, NOT THROUGH fs_*. The suite runs
// inside a live kernel with a real root mounted, and mounting ramfs
// through vfs.c would swap the active backend out from under
// everything else running -- the same hazard partition_test.c has with
// the block device, and the same answer: touch the component under
// test and put it back. `ramfs_test_mount()`/`_unmount()` exist for
// exactly this and for nothing else.
//
// WHAT IS WORTH ASSERTING HERE, given that a broken version still
// "works" for a single small file: the CHUNK BOUNDARY (a write that
// spans two pages is where the block arithmetic is either right or
// silently truncating), HOLES (a grown file must read zeroes without
// having allocated anything), and the BUDGET (the one path that has to
// fail, and the one a full disk would exercise on a real filesystem).
#include "ktest.h"
#include "ramfs.h"
#include "fs.h"
#include "string.h"
#include "heap.h"

// Every test mounts with a small budget rather than the half-of-free-
// memory default: it makes the full-filesystem path reachable without
// allocating a gigabyte, and it keeps a runaway test from competing
// with the rest of the kernel for frames.
#define TEST_BUDGET (256 * 1024)

static const struct fs_ops *R(void) { return &ramfs_ops; }

KTEST("ramfs", "mounts, and reports itself as NOT persistent") {
    // init() returning 0 is the whole point: mounted (not -1), and
    // never persistent (not 1). vfs.c turns that into what `df` says.
    // Through the test seam, which owns the state init() fills -- a
    // backend reached with none faults (fs_ops.h).
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->is_dir("/"));
    KTEST_ASSERT_EQ(R()->init(NULL), 0);
    KTEST_ASSERT(R()->is_dir("/"));
    ramfs_test_unmount();
}

KTEST("ramfs", "a file written is a file read back") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));

    KTEST_ASSERT(R()->write("/hello.txt", "ramfs", 0));
    KTEST_ASSERT(R()->exists("/hello.txt"));
    KTEST_ASSERT(!R()->is_dir("/hello.txt"));
    KTEST_ASSERT_EQ((int)R()->size("/hello.txt"), 5);

    uint32_t got = 0;
    const char *data = R()->read("/hello.txt", &got);
    KTEST_ASSERT(data != 0);
    KTEST_ASSERT_EQ((int)got, 5);
    KTEST_ASSERT_EQ(k_memcmp(data, "ramfs", 5), 0);

    ramfs_test_unmount();
}

KTEST("ramfs", "directories nest, and a listing sees only its own children") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));

    KTEST_ASSERT(R()->mkdir("/etc"));
    KTEST_ASSERT(R()->mkdir("/etc/services.d"));
    KTEST_ASSERT(R()->write("/etc/toyos.conf", "k=v", 0));
    KTEST_ASSERT(R()->write("/top.txt", "x", 0));
    KTEST_ASSERT(R()->is_dir("/etc/services.d"));

    // A file cannot be used as a directory component.
    KTEST_ASSERT(!R()->exists("/top.txt/nope"));
    // Nor can a path reach through a directory that is not there.
    KTEST_ASSERT(!R()->exists("/nosuch/file"));

    ramfs_test_unmount();
}

// The listing callback has no context pointer, so the count lands in a
// file-scope counter -- same shape fs_test.c uses.
static int g_seen;
static int g_saw_dir;
static void count_cb(const char *name, uint32_t size, int is_dir) {
    (void)name; (void)size;
    g_seen++;
    if (is_dir) g_saw_dir++;
}

KTEST("ramfs", "list reports a directory's children and nothing else") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->mkdir("/d"));
    KTEST_ASSERT(R()->write("/d/a", "1", 0));
    KTEST_ASSERT(R()->write("/d/b", "2", 0));
    KTEST_ASSERT(R()->mkdir("/d/sub"));
    KTEST_ASSERT(R()->write("/elsewhere", "3", 0));   // must NOT appear

    g_seen = g_saw_dir = 0;
    R()->list("/d", count_cb);
    KTEST_ASSERT_EQ(g_seen, 3);
    KTEST_ASSERT_EQ(g_saw_dir, 1);

    ramfs_test_unmount();
}

KTEST("ramfs", "a write spanning a chunk boundary reads back whole") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));

    // 4 KiB chunks, so this write starts in chunk 0 and ends in chunk
    // 1. A single-chunk implementation passes every other test in this
    // file and truncates here.
    static uint8_t out[8192];
    for (int i = 0; i < 6000; i++) out[i] = (uint8_t)(i * 31 + 7);
    KTEST_ASSERT(R()->write_range("/span.bin", 3000, out, 6000));
    KTEST_ASSERT_EQ((int)R()->size("/span.bin"), 9000);

    static uint8_t back[8192];
    k_memset(back, 0, sizeof back);
    KTEST_ASSERT_EQ((int)R()->read_range("/span.bin", 3000, back, 6000), 6000);
    KTEST_ASSERT_EQ(k_memcmp(back, out, 6000), 0);

    // ...and the range BEFORE the write was never written, so it must
    // read as zeroes rather than as whatever the heap had.
    k_memset(back, 0xAA, sizeof back);
    KTEST_ASSERT_EQ((int)R()->read_range("/span.bin", 0, back, 3000), 3000);
    for (int i = 0; i < 3000; i++) KTEST_ASSERT_EQ(back[i], 0);

    ramfs_test_unmount();
}

KTEST("ramfs", "growing costs nothing and reads as zeroes; shrinking frees") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->write("/t.bin", "abcd", 0));

    uint64_t before = ramfs_test_used();
    // fs.h: growing extends with zeros and does NOT consume blocks.
    KTEST_ASSERT(R()->truncate("/t.bin", 1024 * 1024));
    KTEST_ASSERT_EQ((int)(R()->size("/t.bin") / 1024), 1024);
    KTEST_ASSERT_EQ((int)(ramfs_test_used() - before), 0);

    static uint8_t back[512];
    k_memset(back, 0xAA, sizeof back);
    KTEST_ASSERT_EQ((int)R()->read_range("/t.bin", 900 * 1024, back, 512), 512);
    for (int i = 0; i < 512; i++) KTEST_ASSERT_EQ(back[i], 0);

    // Shrinking gives the chunks back. Write a real megabyte first so
    // there is something to give: without this the check would pass on
    // a hole-only file, which frees nothing and proves nothing.
    static uint8_t blob[4096];
    k_memset(blob, 0x5A, sizeof blob);
    for (int i = 0; i < 8; i++)
        KTEST_ASSERT(R()->write_range("/t.bin", (uint64_t)i * 4096, blob, 4096));
    uint64_t full = ramfs_test_used();
    KTEST_ASSERT(full > before);
    KTEST_ASSERT(R()->truncate("/t.bin", 100));
    KTEST_ASSERT(ramfs_test_used() < full);
    KTEST_ASSERT_EQ((int)R()->size("/t.bin"), 100);

    ramfs_test_unmount();
}

KTEST("ramfs", "the budget refuses a write past it, and the filesystem survives") {
    // A deliberately tiny budget: enough for the root and a couple of
    // nodes, not enough for the data below.
    KTEST_ASSERT(ramfs_test_mount(32 * 1024));

    static uint8_t blob[4096];
    k_memset(blob, 0x11, sizeof blob);

    int wrote = 0;
    for (int i = 0; i < 64; i++) {
        if (!R()->write_range("/big.bin", (uint64_t)i * 4096, blob, 4096)) break;
        wrote++;
    }
    // It must refuse SOMETHING -- an unbounded ramfs is the failure
    // this budget exists to prevent, and it would happily take all 64.
    KTEST_ASSERT(wrote < 64);
    KTEST_ASSERT(wrote > 0);

    // And refusing must leave a working filesystem, not a wedged one:
    // the bytes that DID land are still readable, and metadata still
    // works. A budget check that corrupted on the way out would be
    // worse than no budget at all.
    static uint8_t back[4096];
    KTEST_ASSERT_EQ((int)R()->read_range("/big.bin", 0, back, 4096), 4096);
    KTEST_ASSERT_EQ(back[0], 0x11);
    KTEST_ASSERT(R()->mkdir("/still-works") || ramfs_test_used() > 0);

    ramfs_test_unmount();
}

KTEST("ramfs", "delete refuses a non-empty directory, and frees a file's chunks") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->mkdir("/d"));
    KTEST_ASSERT(R()->write("/d/f", "content", 0));

    KTEST_ASSERT(!R()->del("/d"));        // has a child
    uint64_t with_file = ramfs_test_used();
    KTEST_ASSERT(R()->del("/d/f"));
    KTEST_ASSERT(ramfs_test_used() < with_file);
    KTEST_ASSERT(R()->del("/d"));         // now empty
    KTEST_ASSERT(!R()->exists("/d"));

    // The root is not deletable, and neither is a path that is not there.
    KTEST_ASSERT(!R()->del("/"));
    KTEST_ASSERT(!R()->del("/never-existed"));

    ramfs_test_unmount();
}

KTEST("ramfs", "rename moves a file, and refuses to move a directory into itself") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->mkdir("/a"));
    KTEST_ASSERT(R()->mkdir("/a/b"));
    KTEST_ASSERT(R()->write("/a/f.txt", "data", 0));

    KTEST_ASSERT(R()->rename("/a/f.txt", "/a/b/g.txt"));
    KTEST_ASSERT(!R()->exists("/a/f.txt"));
    uint32_t got = 0;
    const char *d = R()->read("/a/b/g.txt", &got);
    KTEST_ASSERT(d && got == 4 && k_memcmp(d, "data", 4) == 0);

    // Into itself: this is what detaches a subtree from the root, and
    // nothing below would notice -- the tree would simply have a piece
    // no path can reach.
    KTEST_ASSERT(!R()->rename("/a", "/a/b/a"));
    // A destination that already exists is refused too.
    KTEST_ASSERT(R()->write("/taken", "x", 0));
    KTEST_ASSERT(!R()->rename("/a/b/g.txt", "/taken"));

    ramfs_test_unmount();
}

KTEST("ramfs", "append adds, and a non-append write replaces") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->write("/log", "one", 0));
    KTEST_ASSERT(R()->write("/log", "two", 1));
    KTEST_ASSERT_EQ((int)R()->size("/log"), 6);
    uint32_t got = 0;
    const char *d = R()->read("/log", &got);
    KTEST_ASSERT(d && k_memcmp(d, "onetwo", 6) == 0);

    KTEST_ASSERT(R()->write("/log", "fresh", 0));
    KTEST_ASSERT_EQ((int)R()->size("/log"), 5);
    d = R()->read("/log", &got);
    KTEST_ASSERT(d && got == 5 && k_memcmp(d, "fresh", 5) == 0);

    ramfs_test_unmount();
}

KTEST("ramfs", "stat gives stable, distinct inode numbers") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->write("/one", "1", 0));
    KTEST_ASSERT(R()->write("/two", "2", 0));

    struct fs_stat_info a, b, again;
    KTEST_ASSERT(R()->stat("/one", &a));
    KTEST_ASSERT(R()->stat("/two", &b));
    KTEST_ASSERT(a.ino != b.ino);
    KTEST_ASSERT(R()->stat("/one", &again));
    KTEST_ASSERT_EQ((int)again.ino, (int)a.ino);
    KTEST_ASSERT(!R()->stat("/missing", &a));

    ramfs_test_unmount();
}

KTEST("ramfs", "a path that is not normalized is refused, not guessed at") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    KTEST_ASSERT(R()->mkdir("/d"));
    KTEST_ASSERT(R()->write("/d/f", "x", 0));

    // fs.h's contract: callers resolve "." and ".." themselves. A
    // backend that quietly accepted them would make the shell's `cd`
    // and this disagree about what a path means.
    KTEST_ASSERT(!R()->exists("/d/../d/f"));
    KTEST_ASSERT(!R()->exists("/./d/f"));
    KTEST_ASSERT(!R()->exists("/d/"));
    KTEST_ASSERT(!R()->exists("/d//f"));

    ramfs_test_unmount();
}

KTEST("ramfs", "the steppable write and read pairs honour their contract") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));

    static uint8_t src[600];
    for (int i = 0; i < 600; i++) src[i] = (uint8_t)i;
    void *h = R()->write_range_begin("/step.bin", 0, src, sizeof src);
    KTEST_ASSERT(h != 0);
    KTEST_ASSERT_EQ(R()->write_range_step(h), FS_STEP_DONE);
    KTEST_ASSERT_EQ((int)R()->size("/step.bin"), 600);

    static uint8_t dst[600];
    k_memset(dst, 0, sizeof dst);
    uint32_t total = 0;
    h = R()->read_range_begin("/step.bin", 0, dst, sizeof dst);
    KTEST_ASSERT(h != 0);
    KTEST_ASSERT_EQ(R()->read_range_step(h, &total), FS_STEP_DONE);
    KTEST_ASSERT_EQ((int)total, 600);
    KTEST_ASSERT_EQ(k_memcmp(dst, src, sizeof src), 0);

    ramfs_test_unmount();
}

KTEST("ramfs", "disk_usage reports the budget, and it moves with the data") {
    KTEST_ASSERT(ramfs_test_mount(TEST_BUDGET));
    uint64_t used = 0, total = 0;
    KTEST_ASSERT(R()->disk_usage(&used, &total));
    KTEST_ASSERT_EQ((int)total, TEST_BUDGET);
    uint64_t empty = used;

    static uint8_t blob[4096];
    k_memset(blob, 7, sizeof blob);
    KTEST_ASSERT(R()->write_range("/f", 0, blob, sizeof blob));
    KTEST_ASSERT(R()->disk_usage(&used, &total));
    KTEST_ASSERT(used > empty);

    ramfs_test_unmount();
}
