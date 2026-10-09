// Every disk's partition table, as queryable FACTS.
//
// TWO CLASSES, and the split is the design. QUERY_PARTTABLE is the
// TABLE (which kind, how many entries, the GPT disk GUID);
// QUERY_PARTITION is a LIST of its entries. A list alone could not
// answer the question this repo's own disk asks, because "a table with
// no partitions" and "no partition table at all" are both zero records
// -- and the second is what a stock disk.img actually is.
//
// EVERY READ HITS THE DISK. partition_read_table() reads LBA 0, and for
// a GPT also LBA 1 and the entry array; nothing here caches it. So
// walking N partitions costs N+1 reads. That is deliberate: a cache
// would be a second copy of on-disk state that has to be invalidated by
// anything that rewrites the table (`mkpart`), and a partition table
// read is a handful of sectors from a diagnostic a person typed. If
// this ever moves somewhere hot, cache it THERE rather than here.
#include "query.h"
#include "partition.h"
#include "block.h" // blkdev_sector_count(blk_root_disk())
#include "string.h"
#include <stddef.h>
#include "initcall.h"

// driver-none: a QUERY provider over the partition table

// One scratch table, filled per call. File-scope rather than a local
// because struct partition_table is well over the kernel's 1 KiB frame
// budget (PART_MAX_ENTRIES entries, each carrying two GUIDs and a
// name), and -Wframe-larger-than would refuse it on the stack. Safe for
// the reason every static here is: the kernel is single-threaded, and
// each function below fills it before reading it.
static struct partition_table g_table;

static uint64_t abi_kind(enum partition_table_kind k) {
    switch (k) {
    case PART_TABLE_MBR: return QUERY_PART_MBR;
    case PART_TABLE_GPT: return QUERY_PART_GPT;
    default:             return QUERY_PART_NONE;
    }
}

// EVERY WHOLE DISK, THE BOOT DISK FIRST. Record 0 of both classes is
// still the boot disk's, so a reader that only asks for record 0 -- or
// walks QUERY_PARTITION while `disk` is empty or the boot disk's --
// gets what it always did; QUERY_FSINFO went per-mount the same way.
static const struct block_device *disk_at(int i) {
    const struct block_device *boot = blk_root_disk();
    if (boot && i == 0) return boot;
    int k = boot ? 1 : 0;
    for (int n = 0; n < blk_device_count(); n++) {
        const struct blk_entry *e = blk_device_at(n);
        if (!e || e->parent != e->dev || e->dev == boot) continue;
        if (k++ == i) return e->dev;
    }
    return 0;
}

static int parttable_count(void) {
    int n = 0;
    while (disk_at(n)) n++;
    return n ? n : 1;   // no disk at all still answers "no table"
}

static int parttable_fill(int index, void *out) {
    const struct block_device *disk = disk_at(index);
    if (!disk && index != 0) return 0;
    struct query_parttable *t = out;
    k_memset(t, 0, sizeof *t);
    if (!disk) return 1;
    // A read FAILURE and "no table here" both leave kind NONE, and that
    // is the honest answer either way: this reports what is on the
    // disk, and an unreadable disk has nothing to report.
    if (!partition_read_table_of(disk, &g_table)) g_table.kind = PART_TABLE_NONE, g_table.entry_count = 0;
    t->kind = abi_kind(g_table.kind);
    t->entry_count = (uint64_t)g_table.entry_count;
    // The disk, not a mounted volume: a partition window is never asked.
    t->disk_sectors = (uint64_t)blkdev_sector_count(disk);
    t->block_size = blkdev_block_size(disk);
    k_strlcpy(t->disk, blk_device_name(disk), sizeof t->disk);
    if (g_table.kind == PART_TABLE_GPT) {
        k_memcpy(t->disk_guid, g_table.disk_guid, sizeof t->disk_guid);
    }
    return 1;
}

// Which disk's table holds entry `index` of the flat list, read into
// g_table; *first is that disk's first index. A walk re-reads a table
// per record -- a handful of sectors per disk, for a diagnostic.
static const struct block_device *entry_disk(int index, int *first) {
    int base = 0;
    for (int d = 0;; d++) {
        const struct block_device *disk = disk_at(d);
        if (!disk) return 0;
        if (!partition_read_table_of(disk, &g_table)) continue;
        if (index < base + g_table.entry_count) {
            *first = base;
            return disk;
        }
        base += g_table.entry_count;
    }
}

static int partition_count(void) {
    int n = 0;
    for (int d = 0; disk_at(d); d++)
        if (partition_read_table_of(disk_at(d), &g_table)) n += g_table.entry_count;
    return n;
}

static int partition_fill(int index, void *out) {
    int first = 0;
    const struct block_device *disk = index >= 0 ? entry_disk(index, &first) : 0;
    if (!disk) return 0;
    const struct partition_entry *e = &g_table.entries[index - first];
    struct query_partition *p = out;
    k_memset(p, 0, sizeof *p);

    p->kind = abi_kind(g_table.kind);
    k_strlcpy(p->disk, blk_device_name(disk), sizeof p->disk);
    p->number = (uint64_t)(index - first + 1);
    if (g_table.kind == PART_TABLE_GPT) {
        p->lba_start = e->gpt_lba_start;
        // GPT stores an inclusive END; the ABI carries a LENGTH,
        // because every caller wants a size and half of them would get
        // the +1 wrong. Guarded rather than assumed: a corrupt entry
        // with end < start would otherwise underflow to an enormous
        // partition.
        p->lba_count = e->gpt_lba_end >= e->gpt_lba_start
                     ? e->gpt_lba_end - e->gpt_lba_start + 1 : 0;
        k_memcpy(p->type_guid, e->gpt_type_guid, sizeof p->type_guid);
        k_memcpy(p->unique_guid, e->gpt_unique_guid, sizeof p->unique_guid);
        k_strlcpy(p->name, e->gpt_name, sizeof p->name);
    } else {
        p->lba_start = e->mbr_lba_start;
        p->lba_count = e->mbr_num_sectors;
        p->mbr_type = e->mbr_type;
    }
    return 1;
}

static const struct query_field parttable_fields[] = {
    QUERY_FIELD(struct query_parttable, kind,        QUERY_TYPE_U64),
    QUERY_FIELD(struct query_parttable, entry_count, QUERY_TYPE_U64),
};

static const struct query_provider parttable_provider = {
    .cls = QUERY_PARTTABLE,
    .name = "parttable",
    .record_size = sizeof(struct query_parttable),
    .flags = QUERY_F_LIST,   // one per disk, the boot disk first
    .count = parttable_count,
    .fill = parttable_fill,
    .fields = parttable_fields,
    .field_count = sizeof parttable_fields / sizeof parttable_fields[0],
};

// No named fields: a LIST is not addressable as a flat name, because an
// index baked into one means a different record a second later.
static const struct query_provider partition_provider = {
    .cls = QUERY_PARTITION,
    .name = "partition",
    .record_size = sizeof(struct query_partition),
    .flags = QUERY_F_LIST,
    .count = partition_count,
    .fill = partition_fill,
    .fields = 0,
    .field_count = 0,
};

void partition_query_init(void) {
    query_register(&parttable_provider);
    query_register(&partition_provider);
}
INITCALL(partition_query_init, INIT_QUERY);
