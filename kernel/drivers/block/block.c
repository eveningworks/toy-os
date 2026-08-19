// The block-device registry -- see kernel/include/kernel/block.h for
// why a filesystem talks to this rather than to a disk driver.
#include "block.h"
#include "fault_inject.h"
#include "klog.h"
#include "kfmt.h" // klog_printf
#include <stddef.h>

static const struct block_device *g_dev;

int blk_register(const struct block_device *dev) {
    if (!dev) { g_dev = NULL; return 1; }

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
    klog_printf("block: %s active (%u sectors)\n", dev->name, dev->sector_count());
    return 1;
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

int blk_trim_supported(void) {
    return g_dev && (g_dev->caps & BLK_CAP_TRIM);
}

int blk_trim(uint32_t lba, uint32_t count) {
    if (!blk_trim_supported()) return 0;
    return g_dev->trim(lba, count);
}
