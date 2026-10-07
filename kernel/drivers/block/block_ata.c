// The ATA disk, as a block device. A thin adapter, deliberately: the
// driver keeps every bit of its behaviour (DMA, retries, TRIM, the
// `ata nodma` switch) and this only states which of it the block layer
// may use, and under which capability bit.
#include "block.h"
#include "ata.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`

// driver-none: the block_device shim; ata.c is the driver

static uint64_t ata_dev_sector_count(const struct block_device *self) { (void)self; return ata_sector_count(); }

static int ata_dev_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    (void)self;
    return blk_fits32(lba, (uint64_t)count) && ata_read_sectors((uint32_t)lba, count, buf);
}

static int ata_dev_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    (void)self;
    return blk_fits32(lba, (uint64_t)count) && ata_write_sectors((uint32_t)lba, count, buf);
}

static int ata_dev_max_xfer(const struct block_device *self) { (void)self; return ata_max_sectors_per_xfer(); }

static int ata_dev_flush(const struct block_device *self) { (void)self; return ata_flush_now(); }

static int ata_dev_trim(const struct block_device *self, uint64_t lba, uint32_t count) {
    (void)self;
    return blk_fits32(lba, count) && ata_trim((uint32_t)lba, count);
}
static int ata_dev_trim_ranges(const struct block_device *self, const struct blk_range *r, int n) {
    (void)self;
    return blk_ranges_fit32(r, n) && ata_trim_ranges(r, n);
}

// TRIM is advertised unconditionally and refused per-call by
// ata_trim_supported() inside ata_trim(). The alternative -- deciding
// the capability bit at registration from ata_trim_supported() -- reads
// the drive's identify data before the driver is necessarily settled,
// and this way the bit means "this device type can do TRIM" while the
// call means "this drive would accept one", which are genuinely
// different questions.
static struct block_device ATA_DEV = {
    .name = "ata",
    .driver = "ata",
    .sector_count = ata_dev_sector_count,
    .read_sectors = ata_dev_read,
    .write_sectors = ata_dev_write,
    .max_sectors_per_xfer = ata_dev_max_xfer,
    .persistent = 1,
    .caps = BLK_CAP_FLUSH | BLK_CAP_TRIM,
    .flush = ata_dev_flush,
    .trim = ata_dev_trim,
    .trim_ranges = ata_dev_trim_ranges,
};

void blk_ata_init(void) {
    // No disk is a normal outcome, not an error: the machine may be
    // booting a live image, or have nothing attached at all. Leaving no
    // device registered is exactly what "RAM-only" means downstream.
    if (!ata_present()) return;
    ATA_DEV.model = ata_model();
    blk_register(&ATA_DEV);
}
