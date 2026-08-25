// The disk's partition table, as queryable FACTS.
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
#include "block.h" // blk_disk_sector_count()
#include "string.h"
#include <stddef.h>

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

static int parttable_count(void) { return 1; } // scalar

static int parttable_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_parttable *t = out;
    k_memset(t, 0, sizeof *t);
    // A read FAILURE and "no table here" both leave kind NONE, and that
    // is the honest answer either way: this reports what is on the
    // disk, and an unreadable disk has nothing to report.
    partition_read_table(&g_table);
    t->kind = abi_kind(g_table.kind);
    t->entry_count = (uint64_t)g_table.entry_count;
    // The disk, not the mounted volume -- blk_disk_sector_count()
    // ignores any partition window, which is the point.
    t->disk_sectors = (uint64_t)blk_disk_sector_count();
    if (g_table.kind == PART_TABLE_GPT) {
        k_memcpy(t->disk_guid, g_table.disk_guid, sizeof t->disk_guid);
    }
    return 1;
}

static int partition_count(void) {
    partition_read_table(&g_table);
    return g_table.entry_count;
}

static int partition_fill(int index, void *out) {
    partition_read_table(&g_table);
    if (index < 0 || index >= g_table.entry_count) return 0;
    const struct partition_entry *e = &g_table.entries[index];
    struct query_partition *p = out;
    k_memset(p, 0, sizeof *p);

    p->kind = abi_kind(g_table.kind);
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
    .flags = 0, // scalar
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
