// A disk with 4096-byte logical sectors, end to end: the block layer's
// alignment rule, the partial-block helpers, a GPT written at 4K
// geometry, and TFS3 and FAT32 formatted, mounted and read back on it.
//
// THE DEVICE REFUSES A MISALIGNED TRANSFER ITSELF, the way QEMU answers
// one with IOERR, and COUNTS it. That count is the oracle: the block
// layer refusing first is one line of code, and a test that only asked
// "did the filesystem work" would pass with that line deleted as long as
// nothing ever issued a sub-block request. Every test asserts the count
// stayed zero, so a consumer that bypassed blkdev_*_partial() shows up
// as a named failure rather than as corruption on real hardware.
#include "ktest.h"
#include "block.h"
#include "partition.h"
#include "tfs3.h"
#include "fat32.h"
#include "fs.h"
#include "mount.h"   // mount_scratch_begin/end -- each volume owns its state
#include "heap.h"
#include "string.h"
#include "swap.h"
#include "pmm.h"
#include "scheduler.h"   // preemption off while swap points at the test device

#define D4_SECTORS 16384u               // 8 MiB, in 512-byte units
#define D4_BYTES   (D4_SECTORS * 512u)
#define D4_SPB     8u

static uint8_t *g_d4;
static int g_misaligned;                 // requests that reached the device misaligned

static int d4_ok(uint32_t lba, int count) {
    if (!g_d4 || count <= 0) return 0;
    if ((lba | (uint32_t)count) & (D4_SPB - 1)) { g_misaligned++; return 0; }
    return (uint64_t)lba + (uint64_t)count <= D4_SECTORS;
}
static uint32_t d4_count(void) { return D4_SECTORS; }
static int d4_read(uint32_t lba, int count, void *buf) {
    if (!d4_ok(lba, count)) return 0;
    k_memcpy(buf, g_d4 + (uint64_t)lba * 512, (uint32_t)count * 512);
    return 1;
}
static int d4_write(uint32_t lba, int count, const void *buf) {
    if (!d4_ok(lba, count)) return 0;
    k_memcpy(g_d4 + (uint64_t)lba * 512, buf, (uint32_t)count * 512);
    return 1;
}
static int d4_xfer(void) { return 64; }

static const struct block_device D4_DEV = {
    .name = "d4test", .sector_count = d4_count, .read_sectors = d4_read,
    .write_sectors = d4_write, .max_sectors_per_xfer = d4_xfer,
    .block_size = 4096,
};

static int d4_fresh(void) {
    if (!g_d4) g_d4 = kmalloc(D4_BYTES);
    if (!g_d4) return 0;
    k_memset(g_d4, 0, D4_BYTES);
    g_misaligned = 0;
    return 1;
}

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

KTEST("block4k", "a sub-block transfer is refused, and never reaches the device") {
    if (!d4_fresh()) KTEST_SKIP("could not allocate an 8 MiB volume");
    static uint8_t buf[4096 + 16];
    k_memset(buf, 0xA5, sizeof buf);
    KTEST_ASSERT(!blkdev_read_sectors(&D4_DEV, 1, 1, buf));
    KTEST_ASSERT(!blkdev_read_sectors(&D4_DEV, 0, 1, buf));   // aligned start, short
    KTEST_ASSERT(!blkdev_write_sectors(&D4_DEV, 8, 4, buf));
    KTEST_ASSERT_EQ(g_misaligned, 0);
    KTEST_ASSERT(blkdev_read_sectors(&D4_DEV, 8, 8, buf));
    KTEST_ASSERT_EQ((int)buf[4096], 0xA5);                    // exactly one block landed
    KTEST_ASSERT_EQ((int)blkdev_block_sectors(&D4_DEV), 8);
    KTEST_ASSERT_EQ(blkdev_max_sectors_per_xfer(&D4_DEV) % 8, 0);
}

// Each 512-byte sector holds its own number in every byte, so a read of
// the wrong slice of the block cannot come back looking right.
KTEST("block4k", "a partial read and write touch exactly their own sectors") {
    if (!d4_fresh()) KTEST_SKIP("could not allocate an 8 MiB volume");
    for (uint32_t s = 0; s < 32; s++) k_memset(g_d4 + s * 512, (int)s, 512);

    static uint8_t two[1024];
    KTEST_ASSERT(blkdev_read_partial(&D4_DEV, 13, 2, two));   // straddles blocks 1 and 2
    KTEST_ASSERT_EQ((int)two[0], 13);
    KTEST_ASSERT_EQ((int)two[1023], 14);

    static uint8_t one[512];
    k_memset(one, 0xEE, sizeof one);
    KTEST_ASSERT(blkdev_write_partial(&D4_DEV, 5, 1, one));
    KTEST_ASSERT_EQ((int)g_d4[5 * 512], 0xEE);
    KTEST_ASSERT_EQ((int)g_d4[4 * 512 + 511], 4);             // neighbours kept
    KTEST_ASSERT_EQ((int)g_d4[6 * 512], 6);
    KTEST_ASSERT_EQ((int)g_d4[7 * 512 + 511], 7);
    KTEST_ASSERT_EQ(g_misaligned, 0);
}

// The table's own LBAs count 4K blocks (the UEFI spec), while the
// kernel's partition_table counts 512-byte sectors. Checked in the RAW
// bytes, because a writer and reader that agree on the wrong unit round-
// trip perfectly.
KTEST("block4k", "a GPT on a 4K disk has 4K geometry and round-trips in 512-byte units") {
    if (!d4_fresh()) KTEST_SKIP("could not allocate an 8 MiB volume");
    static struct partition_table in, out;
    k_memset(&in, 0, sizeof in);
    in.kind = PART_TABLE_GPT;
    in.entry_count = 1;
    in.entries[0].gpt_lba_start = 2048;
    in.entries[0].gpt_lba_end = 2048 + 4096 - 1;
    k_strlcpy(in.entries[0].gpt_name, "four-k", sizeof in.entries[0].gpt_name);
    partition_fill_defaults(&in);
    KTEST_ASSERT(partition_write_table_of(&D4_DEV, &in));

    const uint32_t blocks = D4_SECTORS / D4_SPB;
    const uint8_t *hdr = g_d4 + 4096;                       // block 1, not byte 512
    KTEST_ASSERT(k_memcmp(hdr, "EFI PART", 8) == 0);
    KTEST_ASSERT(k_memcmp(g_d4 + 512, "EFI PART", 8) != 0);
    KTEST_ASSERT_EQ((int)le64(hdr + 24), 1);                // my LBA
    KTEST_ASSERT_EQ((int)le64(hdr + 32), (int)(blocks - 1));// backup, last block
    KTEST_ASSERT_EQ((int)le64(hdr + 40), 6);                // first usable: 2 + 16K/4K
    KTEST_ASSERT_EQ((int)le64(hdr + 48), (int)(blocks - 6));// last usable
    KTEST_ASSERT_EQ((int)le64(hdr + 72), 2);                // entry array
    KTEST_ASSERT(k_memcmp(g_d4 + (uint64_t)(blocks - 1) * 4096, "EFI PART", 8) == 0);
    KTEST_ASSERT_EQ((int)le64(g_d4 + 2 * 4096 + 32), 256);  // entry start, in blocks
    KTEST_ASSERT_EQ((int)le64(g_d4 + 2 * 4096 + 40), 256 + 512 - 1);
    KTEST_ASSERT_EQ((int)g_d4[510], 0x55);                  // protective MBR

    KTEST_ASSERT(partition_read_table_of(&D4_DEV, &out));
    KTEST_ASSERT_EQ((int)out.kind, (int)PART_TABLE_GPT);
    KTEST_ASSERT_EQ(out.entry_count, 1);
    KTEST_ASSERT_EQ((int)out.entries[0].gpt_lba_start, 2048);
    KTEST_ASSERT_EQ((int)out.entries[0].gpt_lba_end, 2048 + 4096 - 1);
    KTEST_ASSERT_EQ(g_misaligned, 0);
}

KTEST("block4k", "a partition that is not whole 4K blocks is refused") {
    if (!d4_fresh()) KTEST_SKIP("could not allocate an 8 MiB volume");
    static struct partition_table in;
    k_memset(&in, 0, sizeof in);
    in.kind = PART_TABLE_GPT;
    in.entry_count = 1;
    in.entries[0].gpt_lba_start = 2049;
    in.entries[0].gpt_lba_end = 2049 + 800 - 1;
    partition_fill_defaults(&in);
    const char *why = "";
    KTEST_ASSERT(!partition_validate_on(&D4_DEV, &in, &why));
    KTEST_ASSERT(!partition_write_table_of(&D4_DEV, &in));
    KTEST_ASSERT_EQ((int)g_d4[510], 0);                     // nothing written
    KTEST_ASSERT(blk_part_create(&D4_DEV, 2049, 800, 1) == NULL);
    KTEST_ASSERT(blk_part_create(&D4_DEV, 2048, 801, 1) == NULL);
}

// Byte `k` of block `i` of the file.
static uint8_t pat(uint32_t i, uint32_t k) { return (uint8_t)(i * 37 + k * 7 + (k >> 9)); }

// Formatted, written, UNMOUNTED, mounted again and read back: the
// second mount reads the superblock and the journal header through the
// partial path, which is where a sub-block read would be refused.
KTEST("block4k", "TFS3 formats, mounts and survives a remount on a 4K-sector disk") {
    if (!d4_fresh()) KTEST_SKIP("could not allocate an 8 MiB volume");
    const struct fs_ops *T = &tfs3_ops;
    static uint8_t blk[4096], back[4096];
    struct fs_scratch sc;
    if (!mount_scratch_begin(T, &sc)) KTEST_SKIP("no backend state");
    int ok = T->format(sc.st, &D4_DEV) && T->init(sc.st, &D4_DEV, 0) >= 0;
    for (uint32_t i = 0; ok && i < 3; i++) {
        for (uint32_t k = 0; k < 4096; k++) blk[k] = pat(i, k);
        ok = T->write_range(sc.st, "/four-k", (uint64_t)i * 4096, blk, 4096);
    }
    if (ok) ok = T->write(sc.st, "/small", "4kn", 0);
    mount_scratch_end(&sc);
    KTEST_ASSERT(ok);

    if (!mount_scratch_begin(T, &sc)) KTEST_SKIP("no backend state");
    int mounted = T->probe(sc.st, &D4_DEV) == 1 && T->init(sc.st, &D4_DEV, 0) >= 0;
    int same = mounted;
    for (uint32_t i = 0; same && i < 3; i++) {
        same = T->read_range(sc.st, "/four-k", (uint64_t)i * 4096, back, 4096) == 4096;
        for (uint32_t k = 0; same && k < 4096; k++) same = back[k] == pat(i, k);
    }
    char s[8] = {0};
    uint32_t got = mounted ? T->read_range(sc.st, "/small", 0, s, sizeof s - 1) : 0;
    mount_scratch_end(&sc);

    KTEST_ASSERT(mounted);
    KTEST_ASSERT(same);
    KTEST_ASSERT_EQ((int)got, 3);
    KTEST_ASSERT_EQ(k_strcmp(s, "4kn"), 0);
    KTEST_ASSERT_EQ(g_misaligned, 0);
}

KTEST("block4k", "FAT32 formats with 4096-byte sectors and reads back across clusters") {
    if (!d4_fresh()) KTEST_SKIP("could not allocate an 8 MiB volume");
    const struct fs_ops *F = &fat32_ops;
    struct fs_scratch sc;
    if (!mount_scratch_begin(F, &sc)) KTEST_SKIP("no backend state");
    int ok = F->format(sc.st, &D4_DEV) && F->init(sc.st, &D4_DEV, 0) == 1;
    // bytes_per_sector in the BPB: the device's block, not 512.
    int bps = g_d4[11] | (g_d4[12] << 8);
    static uint8_t data[10000], back[10000];
    for (uint32_t k = 0; k < sizeof data; k++) data[k] = pat(k >> 12, k);
    if (ok) ok = F->write_range(sc.st, "/SPAN.BIN", 0, data, sizeof data);
    if (F->umount) F->umount(sc.st, &D4_DEV);
    mount_scratch_end(&sc);
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(bps, 4096);

    if (!mount_scratch_begin(F, &sc)) KTEST_SKIP("no backend state");
    int mounted = F->probe(sc.st, &D4_DEV) == 1 && F->init(sc.st, &D4_DEV, 0) == 1;
    uint32_t got = mounted ? F->read_range(sc.st, "/SPAN.BIN", 0, back, sizeof back) : 0;
    if (F->umount) F->umount(sc.st, &D4_DEV);
    mount_scratch_end(&sc);

    KTEST_ASSERT(mounted);
    KTEST_ASSERT_EQ((int)got, (int)sizeof data);
    KTEST_ASSERT(k_memcmp(back, data, sizeof data) == 0);
    KTEST_ASSERT_EQ(g_misaligned, 0);
}

// The swap header is one 512-byte sector: less than a block here, so it
// must go through the partial path, and the page slots after it are
// whole blocks and must not.
KTEST("block4k", "a swap area formats, turns on and round-trips a page on a 4K-sector disk") {
    if (!d4_fresh()) KTEST_SKIP("could not allocate an 8 MiB volume");
    scheduler_preempt_disable();
    if (swap_active()) { scheduler_preempt_enable(); KTEST_SKIP("swap is already on"); }
    uint64_t f = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!f) { scheduler_preempt_enable(); KTEST_SKIP("no frame"); }
    uint8_t *pg = (uint8_t *)(uintptr_t)f;            // identity-mapped
    const char *why = "";
    int formatted = swap_format(&D4_DEV, &why);
    int magic = k_memcmp(g_d4, SWAP_MAGIC, SWAP_MAGIC_N) == 0;
    int on = formatted && swap_on(&D4_DEV, &why);
    int same = 0;
    if (on) {
        uint32_t slot = swap_slot_alloc();
        for (uint32_t k = 0; k < 4096; k++) pg[k] = pat(slot, k);
        int w = slot && swap_write_page(slot, f);
        k_memset(pg, 0, 4096);
        int r = w && swap_read_page(slot, f);
        same = r;
        for (uint32_t k = 0; same && k < 4096; k++) same = pg[k] == pat(slot, k);
        if (slot) swap_slot_free(slot);
        swap_off(&why);
    }
    scheduler_preempt_enable();
    pmm_free_frame(f);
    KTEST_ASSERT(formatted);
    KTEST_ASSERT(magic);
    KTEST_ASSERT(on);
    KTEST_ASSERT(same);
    KTEST_ASSERT_EQ(g_misaligned, 0);
}
