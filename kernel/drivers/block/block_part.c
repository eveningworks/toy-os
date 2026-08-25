// A block device over a WINDOW of another block device -- one
// partition of a disk.
//
// Every LBA is shifted by base_lba and every transfer is bounds-checked
// against the window, so whatever mounts this sees a device that starts
// at sector 0 and ends at the partition's end. That is the whole trick,
// and it is why TFS3 needed no change to be mountable from a partition:
// it was already volume-relative behind a {base_lba, sector_count} seam
// (docs/tfs3-design.md, "Volumes and partitions"), and the volume it is
// handed is now genuinely the partition, so that seam stays at
// {0, blk_sector_count()} and means the right thing.
//
// WHY THE OFFSET LIVES HERE and not in the filesystem: this is where
// Linux and Windows both put it -- Linux gives each partition its own
// `struct block_device` carrying `bd_start_sect`, and the filesystem
// driver never learns partitions exist; Windows stacks `partmgr`
// between the disk driver and the volume. Teaching each backend to add
// an offset itself is the layering both moved away from, and it is work
// that would have to be repeated per backend.
//
// **THE ACTIVE DEVICE IS STILL SINGULAR.** A partition REPLACES its
// parent as the active device rather than sitting beside it, which is
// what keeps this clear of the mount-table work docs/roadmap-details.md's
// "Real mount points" is holding. One partition is mounted at a time,
// exactly as one whole disk was.
#include "block.h"
#include "kfmt.h" // k_snprintf
#include "klog.h"
#include <stddef.h>

// The parent is BORROWED, not copied -- the disk drivers' device
// structs are static and outlive everything here.
static const struct block_device *g_parent;
static uint32_t g_base;
static uint32_t g_sectors;

// Mutable and static, like block_virtio.c's: blk_register() keeps the
// pointer rather than a copy, so the struct has to outlive the call and
// the name has to stay valid for as long as `df` might print it.
static char g_name[24];
static struct block_device PART_DEV;

static uint32_t part_sector_count(void) { return g_sectors; }

// Compared in 64-bit throughout: lba + count overflows a uint32_t for a
// large enough lba, and an overflowed comparison passes. block_ram.c
// learned this one first.
static int part_range_ok(uint32_t lba, int count) {
    if (!g_parent || count <= 0) return 0;
    return (uint64_t)lba + (uint64_t)count <= (uint64_t)g_sectors;
}

static int part_read(uint32_t lba, int count, void *buf) {
    if (!part_range_ok(lba, count)) return 0;
    return g_parent->read_sectors(g_base + lba, count, buf);
}

static int part_write(uint32_t lba, int count, const void *buf) {
    if (!part_range_ok(lba, count)) return 0;
    return g_parent->write_sectors(g_base + lba, count, buf);
}

static int part_max_xfer(void) { return g_parent ? g_parent->max_sectors_per_xfer() : 1; }

// Forwarded unchanged. This flushes the parent's WHOLE cache, not just
// this window -- broader than asked for, and deliberately so: a barrier
// that covers more than it promised is safe, one that covers less is
// the silent data-loss bug blk_flush()'s comment is about.
static int part_flush(void) { return g_parent->flush(); }

// TRIM is the one operation that must be CLAMPED as well as offset. A
// caller handing a count past the window's end would otherwise discard
// the next partition's data -- and TRIM is unrecoverable, so this
// refuses rather than truncating, the same call every parser in this
// tree makes.
static int part_trim(uint32_t lba, uint32_t count) {
    if (!part_range_ok(lba, (int)count)) return 0;
    return g_parent->trim(g_base + lba, count);
}

int blk_part_register(const struct block_device *parent,
                      uint32_t base_lba, uint32_t sectors, int index) {
    if (!parent) return 0;

    // Re-pointing rather than nesting: the boot scan walks the entries
    // by registering each in turn, so by the second one blk_active() is
    // already a partition. A partition of a partition is not a thing
    // this models, and silently building one would put a second offset
    // on every LBA.
    if (parent == &PART_DEV) parent = g_parent;
    if (!parent) return 0;

    if (sectors == 0) {
        klog_write("block: partition refused -- empty window\n");
        return 0;
    }
    if ((uint64_t)base_lba + (uint64_t)sectors > (uint64_t)parent->sector_count()) {
        klog_printf("block: partition refused -- LBA %u+%u past the end of %s (%u sectors)\n",
                    base_lba, sectors, parent->name, parent->sector_count());
        return 0;
    }

    g_parent = parent;
    g_base = base_lba;
    g_sectors = sectors;
    k_snprintf(g_name, sizeof(g_name), "%s%d", parent->name, index);

    PART_DEV.name = g_name;
    PART_DEV.sector_count = part_sector_count;
    PART_DEV.read_sectors = part_read;
    PART_DEV.write_sectors = part_write;
    PART_DEV.max_sectors_per_xfer = part_max_xfer;

    // Every capability is INHERITED, both the bit and the pointer, so
    // blk_register_over()'s both-directions honesty check keeps holding:
    // a partition of a device with no flush must not claim one.
    PART_DEV.persistent = parent->persistent;
    PART_DEV.caps = parent->caps;
    PART_DEV.flush = (parent->caps & BLK_CAP_FLUSH) ? part_flush : NULL;
    PART_DEV.trim  = (parent->caps & BLK_CAP_TRIM)  ? part_trim  : NULL;

    return blk_register_over(&PART_DEV, parent, base_lba);
}
