// Tests for MBR/GPT partition tables (kernel/drivers/partition.c).
//
// TWO KINDS, and the second is the one worth having.
//
// The validator tests are pure logic against the real disk's size --
// they assert the REFUSALS, which is where a table writer does its
// actual work. A writer that encodes correctly and validates loosely
// will happily produce two partitions sharing sectors, and nothing
// downstream would notice until a filesystem in one of them started
// reading the other one's data.
//
// The round-trip test writes a real GPT and reads it back through the
// real parser. That covers, in one assertion set, the CRC32 over both
// the header and the entry array, every field offset in a 92-byte
// header and a 128-byte entry, GPT's mixed-endian GUID layout, the
// INCLUSIVE end LBA, and the UTF-16LE name conversion in both
// directions -- none of which a validator test can see. Before this
// existed, the GPT half was only ever proved by a host-compiled
// harness against a synthetic image (docs/decisions/storage.md, "GPT
// header verification"), because TFS2 overwrote LBA 1 on every boot.
// TFS3 reserves volume blocks 0-7, so that blocker is gone.
//
// IT RUNS ON A RAM DISK, NEVER THE REAL ONE. Writing a partition table
// to the disk this kernel booted from would destroy the filesystem the
// rest of the suite is using. The active block device is swapped for a
// scratch RAM device and PUT BACK, with preemption held off across the
// whole window -- a ring-3 syscall landing in the middle would do its
// filesystem I/O against a 512 KiB RAM disk with no filesystem on it.
#include "ktest.h"
#include "partition.h"
#include "block.h"
#include "heap.h"
#include "scheduler.h"
#include "string.h"

// 1024 sectors. Big enough for a GPT's whole geometry -- 34 at the
// front, 33 at the back -- with room between for two partitions.
#define SCRATCH_SECTORS 1024
#define SCRATCH_BYTES (SCRATCH_SECTORS * 512)

KTEST("partition", "validate refuses partitions that overlap each other") {
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 2;
    t.entries[0].mbr_lba_start = 2048;  t.entries[0].mbr_num_sectors = 1000;
    t.entries[1].mbr_lba_start = 2500;  t.entries[1].mbr_num_sectors = 1000; // starts inside #1
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "validate accepts partitions that merely touch") {
    // The half-open boundary, and the reason the overlap test above is
    // written out rather than as a clever one-liner: an off-by-one in
    // either direction turns "adjacent" into "overlapping" and refuses
    // the most ordinary layout there is.
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 2;
    t.entries[0].mbr_lba_start = 2048;  t.entries[0].mbr_num_sectors = 1000;
    t.entries[1].mbr_lba_start = 3048;  t.entries[1].mbr_num_sectors = 1000; // exactly after #1
    const char *why = "";
    KTEST_ASSERT(partition_validate(&t, &why));
}

KTEST("partition", "validate refuses a partition running past the disk") {
    if (!blk_present()) KTEST_SKIP("no block device");
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 1;
    t.entries[0].mbr_lba_start = blk_disk_sector_count() - 10;
    t.entries[0].mbr_num_sectors = 1000;
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "validate refuses a partition sitting on the table") {
    // LBA 0..33 is the GPT itself. A table that describes a partition
    // there would be describing its own destruction.
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_GPT;
    t.entry_count = 1;
    t.entries[0].gpt_lba_start = 2;
    t.entries[0].gpt_lba_end = 2047;
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "validate refuses a fifth MBR partition") {
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 5;
    for (int i = 0; i < 5; i++) {
        t.entries[i].mbr_lba_start = 2048 + (uint32_t)i * 1000;
        t.entries[i].mbr_num_sectors = 1000;
    }
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "a GPT round-trips through the writer and the parser") {
    void *scratch = kmalloc(SCRATCH_BYTES);
    if (!scratch) KTEST_SKIP("no memory for a scratch disk");

    // Everything from here until the restore runs with the real block
    // device swapped out. Preemption stays off across all of it.
    scheduler_preempt_disable();
    const struct block_device *saved = blk_active();
    int registered = blk_ram_register((uint64_t)(uintptr_t)scratch, SCRATCH_BYTES);

    int wrote = 0, read_back = 0;
    static struct partition_table in, out;

    if (registered) {
        k_memset(&in, 0, sizeof in);
        in.kind = PART_TABLE_GPT;
        in.entry_count = 2;
        in.entries[0].gpt_lba_start = 64;
        in.entries[0].gpt_lba_end = 511;   // INCLUSIVE -- 448 sectors
        k_strlcpy(in.entries[0].gpt_name, "first", sizeof in.entries[0].gpt_name);
        in.entries[1].gpt_lba_start = 512;
        in.entries[1].gpt_lba_end = 900;
        k_strlcpy(in.entries[1].gpt_name, "second", sizeof in.entries[1].gpt_name);

        partition_fill_defaults(&in);
        wrote = partition_write_table(&in);
        if (wrote) read_back = partition_read_table(&out);
    }

    // RESTORED BEFORE ANY ASSERTION. A failing KTEST_ASSERT returns
    // from the test function, so an assert before this line would leave
    // the machine running on a 512 KiB RAM disk with no filesystem --
    // one red test taking every later test with it, and the boot too.
    blk_register(saved);
    scheduler_preempt_enable();
    kfree(scratch);

    KTEST_ASSERT(registered);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(read_back);
    KTEST_ASSERT_EQ(out.kind, PART_TABLE_GPT);
    KTEST_ASSERT(out.gpt_header_valid);   // the header CRC32 checked out
    KTEST_ASSERT_EQ(out.entry_count, 2);

    for (int i = 0; i < 2; i++) {
        KTEST_ASSERT_EQ(out.entries[i].gpt_lba_start, in.entries[i].gpt_lba_start);
        KTEST_ASSERT_EQ(out.entries[i].gpt_lba_end, in.entries[i].gpt_lba_end);
        KTEST_ASSERT_EQ(k_strcmp(out.entries[i].gpt_name, in.entries[i].gpt_name), 0);
        // The GUIDs are the mixed-endian half, and the half most likely
        // to be written one way and read another -- which would look
        // perfectly fine in isolation and match nothing.
        KTEST_ASSERT_EQ(k_memcmp(out.entries[i].gpt_type_guid,
                                 in.entries[i].gpt_type_guid, 16), 0);
        KTEST_ASSERT_EQ(k_memcmp(out.entries[i].gpt_unique_guid,
                                 in.entries[i].gpt_unique_guid, 16), 0);
    }
}

KTEST("partition", "an MBR round-trips, and a GPT disk is not read as one") {
    void *scratch = kmalloc(SCRATCH_BYTES);
    if (!scratch) KTEST_SKIP("no memory for a scratch disk");

    scheduler_preempt_disable();
    const struct block_device *saved = blk_active();
    int registered = blk_ram_register((uint64_t)(uintptr_t)scratch, SCRATCH_BYTES);

    int wrote = 0, read_back = 0;
    static struct partition_table in, out;

    if (registered) {
        k_memset(&in, 0, sizeof in);
        in.kind = PART_TABLE_MBR;
        in.entry_count = 2;
        in.entries[0].mbr_lba_start = 64;  in.entries[0].mbr_num_sectors = 448;
        in.entries[1].mbr_lba_start = 512; in.entries[1].mbr_num_sectors = 389;
        partition_fill_defaults(&in);
        wrote = partition_write_table(&in);
        if (wrote) read_back = partition_read_table(&out);
    }

    blk_register(saved);
    scheduler_preempt_enable();
    kfree(scratch);

    KTEST_ASSERT(registered);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(read_back);
    // MBR, not GPT: the discriminator is the protective 0xEE entry, and
    // a parser that just tried LBA 1 regardless would read whatever was
    // there and could report either answer.
    KTEST_ASSERT_EQ(out.kind, PART_TABLE_MBR);
    KTEST_ASSERT_EQ(out.entry_count, 2);
    KTEST_ASSERT_EQ(out.entries[0].mbr_lba_start, 64);
    KTEST_ASSERT_EQ(out.entries[0].mbr_num_sectors, 448);
    KTEST_ASSERT_EQ(out.entries[1].mbr_lba_start, 512);
}
