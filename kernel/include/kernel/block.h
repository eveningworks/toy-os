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

// Registers `dev` as active while recording what it SITS ON. A plain
// disk is its own whole disk at base 0, which is what blk_register()
// passes; a partition passes its parent and its start LBA.
//
// One entry point rather than a register-then-annotate pair, because a
// second call is a thing to forget, and forgetting it would leave the
// partition table being read out of the mounted partition instead of
// off the disk -- a wrong answer, not an error.
int blk_register_over(const struct block_device *dev,
                      const struct block_device *parent, uint32_t base_lba);

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

// Registers the virtio disk as the active device, if there is one and
// it should have it. Returns 1 if it took the role.
//
// PRECEDENCE, since blk_register() is last-writer-wins and order alone
// decides: virtio-blk is PREFERRED when a virtio disk is attached, and
// ATA is the fallback and the legacy path. `novirtio` on the boot line
// forces ATA. See kernel/fs/vfs.c, where the choice is made in one
// place, and block_virtio.c for the measurements behind it.
int blk_virtio_init(void);

// Registers a RAM-backed device over [base, base + bytes). For a live
// image handed over by the bootloader; see kernel/drivers/block/ram.c.
int blk_ram_register(uint64_t base, uint64_t bytes);

// ---- partitions ----------------------------------------------------
//
// A PARTITION IS A BLOCK DEVICE OVER A WINDOW OF ANOTHER ONE. Every
// LBA is shifted by base_lba and every transfer is bounds-checked
// against the window, so the filesystem above sees a device that
// starts at 0 and ends at the partition's end -- TFS3 needs no change
// at all, because the device it is handed IS the volume.
//
// This is where Linux and Windows both put the offset: Linux gives
// each partition its own `struct block_device` carrying `bd_start_sect`
// and the filesystem driver never learns it exists; Windows stacks
// `partmgr` between the disk driver and the volume. The alternative --
// teaching each filesystem to add an offset itself -- is the layering
// both moved away from, and it would have to be re-done per backend.
//
// The active device stays SINGULAR: a partition device REPLACES its
// parent as the active one rather than sitting beside it, so this
// needs none of the mount-table work docs/roadmap-details.md's "Real
// mount points" is holding. Registering a second partition simply
// re-points the window.
//
// `parent` must be a device that is already known-good (in practice
// blk_active() immediately after a disk driver registered), and it is
// borrowed, not copied -- the disk drivers' device structs are static
// and outlive everything.
//
// Returns 0 and registers nothing if the window is empty or runs past
// the parent's end.
// `index` is the partition's 1-based number, used only for the name
// `df` and the kernel log print ("ata1", "virtio-blk2").
//
// Passing an already-active PARTITION as the parent is safe and is
// what the boot-time scan does: the window re-points to the new
// partition of the same underlying disk rather than nesting.
int blk_part_register(const struct block_device *parent,
                      uint32_t base_lba, uint32_t sectors, int index);

// The disk the partition table lives on: the active device, or its
// PARENT when a partition is active. NULL when nothing is registered.
const struct block_device *blk_whole_disk(void);

// Where the active device starts on that disk -- 0 when the active
// device IS the whole disk.
uint32_t blk_base_lba(void);

// Whole-disk I/O, ignoring any partition window. This is what a
// partition-table reader or writer wants and what a filesystem must
// never use: blk_read_sectors(0) is the volume's first sector, while
// blk_disk_read_sectors(0) is the MBR. Same fault-injection hooks.
uint32_t blk_disk_sector_count(void);
int blk_disk_read_sectors(uint32_t lba, int count, void *buf);
int blk_disk_write_sectors(uint32_t lba, int count, const void *buf);

#endif
