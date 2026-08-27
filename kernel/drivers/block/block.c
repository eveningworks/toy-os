// The block-device registry -- see kernel/include/kernel/block.h for
// why a filesystem talks to this rather than to a disk driver.
#include "block.h"
#include "multiboot.h"
#include "string.h"
#include "fault_inject.h"
#include "klog.h"
#include "kfmt.h" // klog_printf
#include <stddef.h>

static const struct block_device *g_dev;

// What the active device sits on, and where it starts there. For a
// plain disk these are the device itself and 0; for a partition they
// are the parent disk and the partition's first LBA. Kept HERE rather
// than in block_part.c so that "which device does the partition table
// live on" has one answer whatever is mounted.
static const struct block_device *g_whole;
static uint32_t g_base;

static struct blk_entry g_table[BLK_MAX_DEVICES];
static int g_count;

// `<driver><index>` for a disk, `<disk>p<n>` for a partition. The index
// counts devices of the SAME driver already in the table, so a second
// SATA drive would be ahci1 without anything having to track it.
//
// The driver's own `name` is the stem, minus anything that cannot go in
// an identifier a person types at a prompt -- "virtio-blk" becomes
// "virtio", which is also the word the boot line already uses
// (`novirtio`).
static void make_name(char *out, const struct block_device *dev,
                      const struct block_device *parent, uint32_t base_lba) {
    char stem[BLK_NAME_MAX];
    int n = 0;
    for (const char *p = dev->name; *p && n < BLK_NAME_MAX - 1; p++) {
        if (*p == '-') break;   // "virtio-blk" -> "virtio"
        stem[n++] = *p;
    }
    stem[n] = 0;

    if (base_lba || parent != dev) {
        // A partition ALREADY KNOWS ITS NAME -- block_part.c built it
        // from the partition TABLE's index, which is the number a person
        // reads out of `parttable` and types into `root=`. Deriving one
        // here instead would number by slot order and disagree with the
        // table whenever a partition ahead of it is skipped.
        k_strlcpy(out, dev->name, BLK_NAME_MAX);
        return;
    }

    int nth = 0;
    for (int i = 0; i < g_count; i++)
        if (g_table[i].dev == g_table[i].parent &&
            k_strncmp(g_table[i].name, stem, (uint32_t)n) == 0) nth++;
    k_snprintf(out, BLK_NAME_MAX, "%s%d", stem, nth);
}

// Adds `dev`, or returns the entry it already has. Idempotent because
// re-registering the same device is ordinary -- mount.c hands the root
// back and forth, and a probe saves and restores it.
static const struct blk_entry *table_add(const struct block_device *dev,
                                          const struct block_device *parent,
                                          uint32_t base_lba) {
    for (int i = 0; i < g_count; i++)
        if (g_table[i].dev == dev) return &g_table[i];
    if (g_count >= BLK_MAX_DEVICES) return NULL;

    struct blk_entry *e = &g_table[g_count];
    e->dev = dev;
    e->parent = parent;
    e->base_lba = base_lba;
    make_name(e->name, dev, parent, base_lba);
    g_count++;
    return e;
}

int blk_track(const struct block_device *dev,
              const struct block_device *parent, uint32_t base_lba) {
    if (!dev) return 0;
    return table_add(dev, parent ? parent : dev, base_lba) != NULL;
}

int blk_device_count(void) { return g_count; }

const struct blk_entry *blk_device_at(int i) {
    if (i < 0 || i >= g_count) return NULL;
    return &g_table[i];
}

const struct blk_entry *blk_device_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_count; i++)
        if (k_strcmp(g_table[i].name, name) == 0) return &g_table[i];
    return NULL;
}

const char *blk_device_name(const struct block_device *dev) {
    for (int i = 0; i < g_count; i++)
        if (g_table[i].dev == dev) return g_table[i].name;
    return "?";
}

int blk_register(const struct block_device *dev) {
    return blk_register_over(dev, dev, 0);
}

int blk_register_over(const struct block_device *dev,
                      const struct block_device *parent, uint32_t base_lba) {
    if (!dev) { g_dev = NULL; g_whole = NULL; g_base = 0; return 1; }

    if (!dev->name || !dev->sector_count || !dev->read_sectors ||
        !dev->write_sectors || !dev->max_sectors_per_xfer) {
        klog_write("block: refused a device missing a required operation\n");
        return 0;
    }

    // The honesty check, and the reason it is here rather than trusted:
    // a device that claims BLK_CAP_FLUSH and has no flush() would have
    // every barrier silently do nothing, and the journal's two barriers
    // are the whole reason a crash costs a leak rather than a corrupted
    // filesystem. display_driver refuses the same disagreement for the
    // same reason -- see its header, and docs/decisions.md.
    if ((dev->caps & BLK_CAP_FLUSH) && !dev->flush) {
        klog_printf("block: %s claims FLUSH with no flush()\n", dev->name);
        return 0;
    }
    if ((dev->caps & BLK_CAP_TRIM) && !dev->trim) {
        klog_printf("block: %s claims TRIM with no trim()\n", dev->name);
        return 0;
    }
    // And the other direction: an operation with no bit is a capability
    // nobody will ever call, which is a mistake rather than a choice.
    if (dev->flush && !(dev->caps & BLK_CAP_FLUSH)) {
        klog_printf("block: %s has flush() without BLK_CAP_FLUSH\n", dev->name);
        return 0;
    }
    if (dev->trim && !(dev->caps & BLK_CAP_TRIM)) {
        klog_printf("block: %s has trim() without BLK_CAP_TRIM\n", dev->name);
        return 0;
    }

    const struct blk_entry *e = table_add(dev, parent ? parent : dev, base_lba);
    if (!e) {
        klog_printf("block: no room in the device table for %s\n", dev->name);
        return 0;
    }

    g_dev = dev;
    g_whole = parent ? parent : dev;
    g_base = base_lba;
    if (base_lba) {
        klog_printf("block: %s active (%u sectors at LBA %u of %s)\n", e->name,
                    dev->sector_count(), base_lba, blk_device_name(g_whole));
    } else {
        klog_printf("block: %s active (%u sectors)\n", e->name, dev->sector_count());
    }
    return 1;
}

int blk_set_root(const struct block_device *dev) {
    for (int i = 0; i < g_count; i++) {
        if (g_table[i].dev != dev) continue;
        g_dev   = g_table[i].dev;
        g_whole = g_table[i].parent;
        g_base  = g_table[i].base_lba;
        return 1;
    }
    return 0; // not registered -- a `root=` naming something never found
}

const struct block_device *blk_whole_disk(void) { return g_whole; }

uint32_t blk_base_lba(void) { return g_base; }

uint32_t blk_disk_sector_count(void) {
    return g_whole ? g_whole->sector_count() : 0;
}

// The fault-injection hooks are the same ones blk_read_sectors() uses:
// a partition-table read failing under injection is a case worth being
// able to test, and there is no reason for it to be exempt.
int blk_disk_read_sectors(uint32_t lba, int count, void *buf) {
    if (fault_should_fail_block_read()) return 0;
    return g_whole ? g_whole->read_sectors(lba, count, buf) : 0;
}

int blk_disk_write_sectors(uint32_t lba, int count, const void *buf) {
    if (fault_should_fail_block_write()) return 0;
    return g_whole ? g_whole->write_sectors(lba, count, buf) : 0;
}

const struct block_device *blk_active(void) { return g_dev; }

int blk_present(void) { return g_dev != NULL; }

int blk_persistent(void) { return g_dev && g_dev->persistent; }

const char *blk_name(void) { return g_dev ? g_dev->name : "none"; }

uint32_t blk_sector_count(void) { return g_dev ? g_dev->sector_count() : 0; }

// Fault injection lives HERE, not in a driver, so a filesystem error
// path can be tested whatever the filesystem is mounted on. See
// fault_inject.h -- the ATA-specific pair still exists for ATA's own
// write-back cache tests, which sit below this layer.
int blk_read_sectors(uint32_t lba, int count, void *buf) {
    if (fault_should_fail_block_read()) return 0;
    return g_dev ? g_dev->read_sectors(lba, count, buf) : 0;
}

int blk_write_sectors(uint32_t lba, int count, const void *buf) {
    if (fault_should_fail_block_write()) return 0;
    return g_dev ? g_dev->write_sectors(lba, count, buf) : 0;
}

int blk_max_sectors_per_xfer(void) {
    return g_dev ? g_dev->max_sectors_per_xfer() : 1;
}

// RETURNS whether the data is actually durable. It was `void`, and a
// barrier that cannot fail is exactly what a write-back cache turns
// into a silent data-loss bug: the failure of a deferred write surfaces
// HERE, at the flush, long after the write() that returned success.
// TFS3's txn_commit() checks it -- see ata_cache.h.
//
// 1 on a device with no cache, which is correct and not a degradation:
// a RAM device has nothing that can be lost independently of everything
// else. Only a device that HAS a cache and does not flush it would be
// lying, and blk_register() refuses that shape.
int blk_flush(void) {
    if (g_dev && (g_dev->caps & BLK_CAP_FLUSH)) return g_dev->flush();
    return 1;
}

// `notrim` ON THE BOOT LINE STOPS EVERY BACKEND DISCARDING. Gated HERE,
// at the one place every trim decision passes through, rather than in
// each driver -- three of them can discard now and a word that only
// covered one would be a worse answer than none.
//
// It exists to be a one-boot A/B. A discard punches a hole in the host
// image, and a flush after one can be far slower on some host
// filesystems than on others; when a machine starts failing journal
// barriers, "is it the trims?" is otherwise only answerable by
// rebuilding an older kernel. Same reachability argument as `novirtio`
// and `noahci`, pointed at a capability rather than a driver.
static int g_trim_disabled = -1;   // -1 = not yet asked

static int trim_disabled(void) {
    if (g_trim_disabled >= 0) return g_trim_disabled;
    g_trim_disabled = 0;
    const char *cmdline = multiboot_cmdline();
    if (cmdline) {
        for (const char *p = cmdline; (p = k_strstr(p, "notrim")) != 0; p += 6) {
            if (p != cmdline && p[-1] != ' ') continue;
            char after = p[6];
            if (after == 0 || after == ' ') { g_trim_disabled = 1; break; }
        }
    }
    if (g_trim_disabled) klog_write("block: notrim -- discards are disabled for every backend\n");
    return g_trim_disabled;
}

int blk_trim_supported(void) {
    if (trim_disabled()) return 0;
    return g_dev && (g_dev->caps & BLK_CAP_TRIM);
}

int blk_trim(uint32_t lba, uint32_t count) {
    if (!blk_trim_supported()) return 0;
    return g_dev->trim(lba, count);
}

// ---- I/O on a NAMED device ------------------------------------------
//
// Everything above answers for the ACTIVE device. These answer for the
// one the caller was handed at mount time, which is what a filesystem
// mounted anywhere but the root has to use -- see block.h. The
// fault-injection hooks are the same ones, deliberately: an error path
// does not become untestable by being on a second mount.
int blkdev_read_sectors(const struct block_device *dev, uint32_t lba, int count, void *buf) {
    if (fault_should_fail_block_read()) return 0;
    return dev ? dev->read_sectors(lba, count, buf) : 0;
}

int blkdev_write_sectors(const struct block_device *dev, uint32_t lba, int count, const void *buf) {
    if (fault_should_fail_block_write()) return 0;
    return dev ? dev->write_sectors(lba, count, buf) : 0;
}

int blkdev_max_sectors_per_xfer(const struct block_device *dev) {
    return dev ? dev->max_sectors_per_xfer() : 1;
}

// Same contract as blk_flush(): 1 on a device with no cache, because
// there is nothing that can be lost independently of everything else.
int blkdev_flush(const struct block_device *dev) {
    if (dev && (dev->caps & BLK_CAP_FLUSH)) return dev->flush();
    return 1;
}

int blkdev_trim_supported(const struct block_device *dev) {
    // `notrim` covers this path too. Gating only the active device would
    // leave a second mount still discarding, which is exactly the "it
    // only half works" answer a diagnostic switch must not give.
    if (trim_disabled()) return 0;
    return dev && (dev->caps & BLK_CAP_TRIM);
}

int blkdev_trim(const struct block_device *dev, uint32_t lba, uint32_t count) {
    if (!blkdev_trim_supported(dev)) return 0;
    return dev->trim(lba, count);
}

uint32_t blkdev_sector_count(const struct block_device *dev) {
    return dev ? dev->sector_count() : 0;
}
