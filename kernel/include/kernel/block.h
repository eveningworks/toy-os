#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>

// A block device: what a filesystem reads and writes, without knowing
// what is underneath it.
//
// WHY THIS EXISTS
// ---------------
// TFS3 talked to `ata_*` directly. That was fine while a disk was the
// only thing a filesystem could live on, and it stops being fine the
// moment one lives in RAM -- a Live CD mounts a filesystem image the
// bootloader handed over as a GRUB module, with no ATA controller
// involved at all (docs/live-cd-design.md).
//
// The alternative was an `if (live) ... else ...` at every call site,
// which is the shape that rots: one path gets tested and the other is
// discovered broken later. This is the same registry pattern
// `display_driver` and `fs_ops` already use here, for the same reason --
// the implementation swaps and the callers do not notice.
//
// **TFS2 deliberately does NOT use this.** It makes 24 direct `ata_*`
// calls and a live image is always TFS3 (the default format), so
// rewiring a legacy backend to serve a feature it will never carry
// would be cost with no return. It keeps talking to ATA.

// Optional capabilities. Declared, not discovered -- the same honesty
// rule display_driver follows: a device whose bits and function
// pointers disagree is refused at registration rather than found out at
// runtime, because "needs a flush and never gets one" is a corruption
// bug that surfaces long after the mistake.
#define BLK_CAP_FLUSH 0x01
#define BLK_CAP_TRIM  0x02

struct block_device {
    const char *name;   // "ata", "ram" -- what `df` prints

    // Total addressable sectors, 512 bytes each.
    uint32_t (*sector_count)(void);

    // Both return 1 on success, 0 on failure. `count` sectors from
    // `lba`, contiguous.
    int (*read_sectors)(uint32_t lba, int count, void *buf);
    int (*write_sectors)(uint32_t lba, int count, const void *buf);

    // Largest `count` a single transfer may use. A caller that ignores
    // this gets a refused transfer, not a short one.
    int (*max_sectors_per_xfer)(void);

    // Does what is written here SURVIVE A POWER CYCLE? A disk does; a
    // live image in RAM does not. This is where the answer belongs --
    // a filesystem backend cannot tell, it just reads and writes
    // sectors, and fs_is_persistent() reporting "yes" for a live
    // session would be the single most misleading thing this feature
    // could do (see docs/live-cd-design.md).
    int persistent;

    unsigned caps;      // BLK_CAP_*
    int (*flush)(void);                        // BLK_CAP_FLUSH, 1 = durable
    int (*trim)(uint32_t lba, uint32_t count); // BLK_CAP_TRIM
};

// Registers the active device. Returns 0 (and registers nothing) if the
// device's capability bits and function pointers disagree.
int blk_register(const struct block_device *dev);

// The active device, or NULL when nothing is registered -- which is the
// normal state on a machine with no disk and no live image, and is what
// "RAM-only, nothing mounted" means.
const struct block_device *blk_active(void);

// Thin wrappers, so a filesystem never repeats the NULL check. Reads and
// writes return 0 with no device; the two optional operations are no-ops
// rather than errors, exactly as they are on a device that lacks them.
int blk_present(void);
int blk_persistent(void);
const char *blk_name(void);
uint32_t blk_sector_count(void);
int blk_read_sectors(uint32_t lba, int count, void *buf);
int blk_write_sectors(uint32_t lba, int count, const void *buf);
int blk_max_sectors_per_xfer(void);
int blk_flush(void);
int blk_trim_supported(void);
int blk_trim(uint32_t lba, uint32_t count);

// Registers the ATA disk as the active device, if there is one. Called
// at boot before the filesystem mounts.
void blk_ata_init(void);

// Registers a RAM-backed device over [base, base + bytes). For a live
// image handed over by the bootloader; see kernel/drivers/block/ram.c.
int blk_ram_register(uint64_t base, uint64_t bytes);

#endif
