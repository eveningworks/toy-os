// The block device TABLE, as queryable facts -- what `/bin/lsblk` reads.
//
// A LIST and nothing else, deliberately unlike partition_query.c's
// table/entries split. That split exists because "a table with no
// partitions" and "no table at all" are both zero records and mean
// different things; here zero records means one thing only -- no disk
// was found -- and a scalar beside the list would answer a question
// nobody has.
//
// Cheap: this reads the table block.c already holds, with no disk I/O at
// all. The partition provider next door hits the disk on every read;
// this one cannot, because the table is the kernel's own state.
#include "query.h"
#include "block.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"
#include "devevent.h"   // devevent_pci_id -- the controller's name

// driver-none: a QUERY provider over the block table

static int blkdev_count(void) { return blk_device_count(); }

static int blkdev_fill(int index, void *out) {
    const struct blk_entry *e = blk_device_at(index);
    if (!e) return 0;

    struct query_blkdev *d = out;
    k_memset(d, 0, sizeof *d);
    k_strlcpy(d->name, e->name, sizeof d->name);

    // A disk is its own parent in the table; reporting that as a parent
    // would make every disk look like a partition of itself.
    if (e->parent != e->dev)
        k_strlcpy(d->parent, blk_device_name(e->parent), sizeof d->parent);

    d->sectors = e->dev->sector_count(e->dev);
    d->base_lba = e->base_lba;
    d->is_root = (blk_root() == e->dev) ? 1 : 0;
    d->persistent = e->dev->persistent ? 1 : 0;
    d->block_size = blkdev_block_size(e->dev);
    if (e->dev->model) k_strlcpy(d->model, e->dev->model, sizeof d->model);
    if (e->dev->driver) k_strlcpy(d->driver, e->dev->driver, sizeof d->driver);
    if (e->dev->pci) devevent_pci_id(e->dev->pci, d->device_id, sizeof d->device_id);
    return 1;
}

// No named fields: a LIST is not addressable by a flat name, since an
// index baked into one means a different record a second later. Same
// call partition_query.c makes.
static const struct query_provider blkdev_provider = {
    .cls = QUERY_BLKDEV,
    .name = "blkdev",
    .record_size = sizeof(struct query_blkdev),
    .flags = QUERY_F_LIST,
    .count = blkdev_count,
    .fill = blkdev_fill,
    .fields = 0,
    .field_count = 0,
};

void block_query_init(void) {
    query_register(&blkdev_provider);
}
INITCALL(block_query_init, INIT_QUERY);
