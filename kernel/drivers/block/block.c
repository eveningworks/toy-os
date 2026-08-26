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

    g_dev = dev;
    g_whole = parent ? parent : dev;
    g_base = base_lba;
    if (base_lba) {
        klog_printf("block: %s active (%u sectors at LBA %u of %s)\n", dev->name,
                    dev->sector_count(), base_lba, g_whole->name);
    } else {
        klog_printf("block: %s active (%u sectors)\n", dev->name, dev->sector_count());
    }
    return 1;
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
