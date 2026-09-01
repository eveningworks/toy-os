// A block device backed by ordinary memory -- the simplest driver in
// the tree, and it should stay that way.
//
// This is what a LIVE BOOT mounts: GRUB loads a filesystem image as a
// module, the kernel points this at it, and TFS3 mounts it through the
// same code path a disk uses (docs/live-cd-design.md). Reads and writes
// are a memcpy each way, which is the whole implementation.
//
// **Writes go to memory and die with the power**, and that is what a
// live volume is. Nothing here pretends otherwise: there is no flush
// (nothing can be lost independently of everything else) and no TRIM
// (nothing to hand back), so neither capability bit is set and the
// block layer turns both into no-ops. `fs_is_persistent()` reports the
// truth to `df`, `fsck` and the About window with no special-casing,
// which is the property that stops a live session quietly claiming to
// have saved something.
#include "block.h"
#include "klog.h"
#include "string.h"
#include "driver.h" // DRIVER_REGISTER -- `lsdrv`

#define SECTOR_SIZE 512

// The image the bootloader handed over. Identity-mapped like everything
// else below 4 GiB, so a physical address IS the pointer.
static uint8_t *g_base;
static uint32_t g_sectors;

// One transfer's cap. Not a hardware limit here -- there is no
// controller -- but a value the caller can plan around, and keeping it
// finite means a bug that asks for the whole device at once is refused
// rather than memcpy'ing gigabytes.
#define RAM_MAX_XFER 256

static uint32_t ram_sector_count(void) { return g_sectors; }

static int ram_range_ok(uint32_t lba, int count) {
    if (!g_base || count <= 0 || count > RAM_MAX_XFER) return 0;
    // Compared in 64-bit: lba + count overflows a uint32_t for a large
    // enough lba, and an overflowed comparison passes.
    return (uint64_t)lba + (uint64_t)count <= (uint64_t)g_sectors;
}

static int ram_read(uint32_t lba, int count, void *buf) {
    if (!ram_range_ok(lba, count)) return 0;
    k_memcpy(buf, g_base + (uint64_t)lba * SECTOR_SIZE,
             (size_t)count * SECTOR_SIZE);
    return 1;
}

static int ram_write(uint32_t lba, int count, const void *buf) {
    if (!ram_range_ok(lba, count)) return 0;
    k_memcpy(g_base + (uint64_t)lba * SECTOR_SIZE, buf,
             (size_t)count * SECTOR_SIZE);
    return 1;
}

static int ram_max_xfer(void) { return RAM_MAX_XFER; }

static const struct block_device RAM_DEV = {
    .name = "ram",
    .sector_count = ram_sector_count,
    .read_sectors = ram_read,
    .write_sectors = ram_write,
    .max_sectors_per_xfer = ram_max_xfer,
    .persistent = 0,   // the whole point: writes die with the power
    .caps = 0,     // no flush, no TRIM -- see the top comment
};

int blk_ram_register(uint64_t base, uint64_t bytes) {
    if (!base || bytes < SECTOR_SIZE) {
        klog_write("block: ram device refused -- empty image\n");
        return 0;
    }
    g_base = (uint8_t *)(uintptr_t)base;
    g_sectors = (uint32_t)(bytes / SECTOR_SIZE);
    DRIVER_REGISTER("ram", "block");
    driver_bound("ram", RAM_DEV.name);
    return blk_register(&RAM_DEV);
}
