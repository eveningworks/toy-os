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
// {0, sector_count} and means the right thing.
//
// WHY THE OFFSET LIVES HERE and not in the filesystem: this is where
// Linux and Windows both put it -- Linux gives each partition its own
// `struct block_device` carrying `bd_start_sect`, and the filesystem
// driver never learns partitions exist; Windows stacks `partmgr`
// between the disk driver and the volume. Teaching each backend to add
// an offset itself is the layering both moved away from, and it is work
// that would have to be repeated per backend.
//
// **THERE IS A POOL OF THESE NOW, NOT ONE.** It was a single static
// device because the active device was singular and a partition
// REPLACED its parent. Real mount points ended that: two filesystems
// are mounted at once and each needs its own volume alive at the same
// time, so a window is CREATED (blk_part_create) independently of
// whether anything makes it active (blk_part_register). Creating the
// same window twice returns the SAME device rather than burning a slot
// -- a mount and the boot scan both ask for partition 2, and two
// structs describing one window would give the mount table two names
// for one volume.
#include "block.h"
#include "kfmt.h" // k_snprintf
#include "klog.h"
#include <stddef.h>

// driver-none: partitions of a disk another driver drives

// One per partition that anything holds a device for. Four is MBR's
// limit and more than this OS's own disk uses (bios, esp, tfs3); the
// pool is bounded because a `struct block_device` plus its name is
// static storage, and an unbounded one would be a heap allocation with
// a lifetime nobody owns.
#define PART_SLOTS 8

struct part_slot {
    // The parent is BORROWED, not copied -- the disk drivers' device
    // structs are static and outlive everything here.
    const struct block_device *parent;
    uint64_t base;
    uint64_t sectors;
    int used;
    // Mutable and static, like block_virtio.c's: blk_register() keeps
    // the pointer rather than a copy, so the struct has to outlive the
    // call and the name has to stay valid for as long as `df` might
    // print it.
    char name[24];
    struct block_device dev;
};

static struct part_slot g_slots[PART_SLOTS];

// THE SLOT IS RECOVERED FROM THE DEVICE every op is handed: `dev` is a
// member of struct part_slot, so one set of functions serves every
// window.
static struct part_slot *slot_of(const struct block_device *self) {
    return (struct part_slot *)((char *)self - offsetof(struct part_slot, dev));
}

// Compared in 64-bit throughout: an overflowed `lba + count` passes a
// bounds check, and block_ram.c learned that one first.
static int range_ok(const struct part_slot *s, uint64_t lba, uint64_t count) {
    if (!s->parent || count == 0) return 0;
    return lba + count <= s->sectors && lba + count >= lba;
}

static uint64_t part_count(const struct block_device *self) { return slot_of(self)->sectors; }

static int part_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    struct part_slot *s = slot_of(self);
    if (count <= 0 || !range_ok(s, lba, (uint64_t)count)) return 0;
    return s->parent->read_sectors(s->parent, s->base + lba, count, buf);
}

static int part_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    struct part_slot *s = slot_of(self);
    if (count <= 0 || !range_ok(s, lba, (uint64_t)count)) return 0;
    return s->parent->write_sectors(s->parent, s->base + lba, count, buf);
}

static int part_xfer(const struct block_device *self) {
    struct part_slot *s = slot_of(self);
    return s->parent ? s->parent->max_sectors_per_xfer(s->parent) : 1;
}

// Forwarded unchanged. This flushes the parent's WHOLE cache, not just
// this window -- broader than asked for, and deliberately so: a barrier
// that covers more than it promised is safe, one that covers less is
// the silent data-loss bug blk_flush()'s comment is about.
static int part_flush(const struct block_device *self) {
    struct part_slot *s = slot_of(self);
    return s->parent->flush(s->parent);
}

// Shifted into the parent's LBAs IN PLACE and shifted back afterwards:
// the caller's array is the batch, and a copy would need a bound this
// file does not otherwise have.
static int part_batch(const struct block_device *self, struct blk_io *io, int n) {
    struct part_slot *s = slot_of(self);
    for (int i = 0; i < n; i++)
        if (io[i].count == 0 || !range_ok(s, io[i].lba, io[i].count)) return 0;
    for (int i = 0; i < n; i++) io[i].lba += s->base;
    int ok = s->parent->submit_batch(s->parent, io, n);
    for (int i = 0; i < n; i++) io[i].lba -= s->base;
    return ok;
}

// TRIM is the one operation that must be CLAMPED as well as offset. A
// caller handing a count past the window's end would otherwise discard
// the next partition's data -- and TRIM is unrecoverable, so this
// refuses rather than truncating, the same call every parser in this
// tree makes.
static int part_trim(const struct block_device *self, uint64_t lba, uint32_t count) {
    struct part_slot *s = slot_of(self);
    if (!range_ok(s, lba, count)) return 0;
    return s->parent->trim(s->parent, s->base + lba, count);
}

// The list form: every run checked against the window BEFORE any is
// sent, then offset in batches of a DSM block's worth.
static int part_trim_ranges(const struct block_device *self, const struct blk_range *r, int n) {
    struct part_slot *s = slot_of(self);
    const struct block_device *p = s->parent;
    for (int i = 0; i < n; i++)
        if (!range_ok(s, r[i].lba, r[i].count)) return 0;
    if (!p->trim_ranges) {
        int ok = 1;
        for (int i = 0; i < n; i++) ok &= p->trim(p, s->base + r[i].lba, r[i].count) ? 1 : 0;
        return ok;
    }
    // Half a DSM block per call: a 64-bit range is 16 bytes, and a whole
    // block's worth on the stack is the kernel's entire frame budget.
    struct blk_range out[BLK_DSM_ENTRIES / 2];
    int ok = 1;
    for (int i = 0; i < n; ) {
        int k = 0;
        for (; k < BLK_DSM_ENTRIES / 2 && i < n; k++, i++)
            out[k] = (struct blk_range){ s->base + r[i].lba, r[i].count };
        ok &= p->trim_ranges(p, out, k) ? 1 : 0;
    }
    return ok;
}

int blk_part_release(const struct block_device *dev) {
    for (int i = 0; i < PART_SLOTS; i++) {
        if (!g_slots[i].used || &g_slots[i].dev != dev) continue;
        if (!blk_untrack(dev)) return 0;   // in use, or not named
        g_slots[i].used = 0;
        g_slots[i].parent = NULL;
        g_slots[i].base = g_slots[i].sectors = 0;
        g_slots[i].name[0] = '\0';
        return 1;
    }
    return 0;
}

const struct block_device *blk_part_create(const struct block_device *parent,
                                           uint64_t base_lba, uint64_t sectors,
                                           int index) {
    if (!parent) return NULL;

    // Re-pointing rather than nesting: the boot scan walks the entries
    // by creating each in turn, so the caller may well hand back a
    // partition it got from here. A partition of a partition is not a
    // thing this models, and silently building one would put a second
    // offset on every LBA.
    for (int i = 0; i < PART_SLOTS; i++) {
        if (parent == &g_slots[i].dev) {
            base_lba += g_slots[i].base;
            parent = g_slots[i].parent;
            break;
        }
    }
    if (!parent) return NULL;

    if (sectors == 0) {
        klog_write(KLOG_ERR "block: partition refused -- empty window\n");
        return NULL;
    }
    // On a 4K-sector disk a window must be whole blocks, or every
    // transfer through it would be refused as misaligned.
    uint32_t spb = blkdev_block_sectors(parent);
    if (((base_lba | sectors) & (spb - 1)) != 0) {
        klog_printf(KLOG_ERR "block: partition refused -- LBA %llu+%llu is not whole %u-byte blocks\n",
                    (unsigned long long)base_lba, (unsigned long long)sectors,
                    blkdev_block_size(parent));
        return NULL;
    }
    if (base_lba + sectors > parent->sector_count(parent) || base_lba + sectors < base_lba) {
        klog_printf(KLOG_ERR "block: partition refused -- LBA %llu+%llu past the end of %s (%llu sectors)\n",
                    (unsigned long long)base_lba, (unsigned long long)sectors, parent->name,
                    (unsigned long long)parent->sector_count(parent));
        return NULL;
    }

    // THE SAME WINDOW IS THE SAME DEVICE. Without this the boot scan's
    // pass and a later `mount` of the same partition would produce two
    // structs describing one volume, and the mount table's "is this
    // device already mounted?" check -- which compares pointers -- would
    // answer no while the answer is plainly yes.
    int free_slot = -1;
    for (int i = 0; i < PART_SLOTS; i++) {
        if (g_slots[i].used && g_slots[i].parent == parent &&
            g_slots[i].base == base_lba && g_slots[i].sectors == sectors) {
            return &g_slots[i].dev;
        }
        if (!g_slots[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        klog_write(KLOG_ERR "block: partition refused -- no free window slots\n");
        return NULL;
    }

    struct part_slot *s = &g_slots[free_slot];
    s->parent = parent;
    s->base = base_lba;
    s->sectors = sectors;
    s->used = 1;
    // `<disk>p<index>`, and the index is the PARTITION TABLE's, not a
    // count of how many slots are in use -- `root=ata0p3` has to mean
    // the third entry in the table, which is what a person reading
    // `parttable` will type. Naming by slot order made partition 3 of
    // this disk "ata0p1", because the two firmware partitions ahead of
    // it are skipped.
    //
    // The DISK's name comes from the block table rather than from
    // parent->name, which is the driver's stem ("ata") and not an
    // identity ("ata0"); the fallback keeps a partition of an
    // unregistered parent nameable rather than calling it "?p3".
    const char *pname = blk_device_name(parent);
    if (pname[0] == '?') pname = parent->name;
    k_snprintf(s->name, sizeof(s->name), "%sp%d", pname, index);

    s->dev.name = s->name;
    s->dev.sector_count = part_count;
    s->dev.read_sectors = part_read;
    s->dev.write_sectors = part_write;
    s->dev.max_sectors_per_xfer = part_xfer;

    // Every capability is INHERITED, both the bit and the pointer, so
    // blk_register_over()'s both-directions honesty check keeps holding:
    // a partition of a device with no flush must not claim one.
    s->dev.persistent = parent->persistent;
    s->dev.block_size = parent->block_size;
    s->dev.caps = parent->caps;
    s->dev.flush = (parent->caps & BLK_CAP_FLUSH) ? part_flush : NULL;
    s->dev.trim  = (parent->caps & BLK_CAP_TRIM)  ? part_trim  : NULL;
    s->dev.trim_ranges = (parent->caps & BLK_CAP_TRIM) ? part_trim_ranges : NULL;
    s->dev.submit_batch = parent->submit_batch ? part_batch : NULL;

    // Into the block table even though nothing is being made active --
    // the table is what gives a device its NAME, and an unnamed device
    // cannot be mounted by name. /boot's partition arrives here and
    // never through blk_register().
    blk_track(&s->dev, parent, base_lba);
    return &s->dev;
}

const struct block_device *blk_part_parent(const struct block_device *dev,
                                           uint64_t *out_base) {
    for (int i = 0; i < PART_SLOTS; i++) {
        if (g_slots[i].used && dev == &g_slots[i].dev) {
            if (out_base) *out_base = g_slots[i].base;
            return g_slots[i].parent;
        }
    }
    return NULL;
}

int blk_part_register(const struct block_device *parent,
                      uint64_t base_lba, uint64_t sectors, int index) {
    const struct block_device *dev = blk_part_create(parent, base_lba, sectors, index);
    if (!dev) return 0;
    uint64_t base = base_lba;
    const struct block_device *real_parent = blk_part_parent(dev, &base);
    return blk_register_over(dev, real_parent ? real_parent : parent, base);
}
