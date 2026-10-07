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
// **EVERY FILESYSTEM GOES THROUGH THIS NOW.** TFS2 deliberately did
// not -- 24 direct `ata_*` calls -- and was removed rather than
// rewired; `fs_ops.volume_relative` is what stopped the partition scan
// offering it a window it would have ignored. A future backend that
// wants a live image, a RAM disk or a partition has to come through
// here, and declaring otherwise is how it says so.

// Optional capabilities. Declared, not discovered -- the same honesty
// rule display_driver follows: a device whose bits and function
// pointers disagree is refused at registration rather than found out at
// runtime, because "needs a flush and never gets one" is a corruption
// bug that surfaces long after the mistake.
#define BLK_CAP_FLUSH 0x01
#define BLK_CAP_TRIM  0x02

// One run of sectors to discard.
struct pci_device;
struct blk_range { uint64_t lba; uint32_t count; };

// Whether [lba, lba + count) is addressable by a driver whose own
// interface is 32-bit -- an adapter refuses rather than truncating.
static inline int blk_fits32(uint64_t lba, uint64_t count) {
    return lba + count <= 0x100000000ull;
}
// The same for a list that reaches such a driver whole.
static inline int blk_ranges_fit32(const struct blk_range *r, int n) {
    for (int i = 0; i < n; i++) if (!blk_fits32(r[i].lba, r[i].count)) return 0;
    return 1;
}

// One transfer of a batch (blkdev_submit_batch()). `buf` is written TO
// for a read and read FROM for a write, and belongs to the caller until
// the batch returns. `ok` is the device's answer for this one.
struct blk_io {
    uint64_t lba;
    uint16_t count;      // sectors, at most the device's max per transfer
    uint8_t  write;
    int8_t   ok;
    void    *buf;
};

struct block_device {
    const char *name;   // "ata", "ram" -- what `df` prints

    // The DRIVER behind this device -- "ata", "virtio-blk", "ahci".
    // Distinct from `name`, which names the device: one driver may
    // present several. blk_register_over() reports it to `lsdrv`, so a
    // driver that fills this in cannot then forget to say so. NULL for
    // a partition, whose driver is the whole disk's.
    const char *driver;

    // WHAT THE DISK IS AND WHAT IT HANGS OFF, for Device Manager and
    // `lsblk`. Both optional: NULL for a partition (its disk's), a RAM
    // disk, or a driver that does not know.
    const char *model;               // "SAMSUNG MZNLN128HAHQ-000H1"
    const struct pci_device *pci;    // its controller

    // EVERY OP IS HANDED THE DEVICE IT WAS CALLED THROUGH (`self`), so
    // one function can serve several devices -- each partition, each NVMe
    // namespace -- and recover its own state from the pointer. LBAs are
    // 64-bit; a driver that addresses less refuses past its end.

    // Total addressable sectors, 512 bytes each -- whatever `block_size`
    // says. See block_size below.
    uint64_t (*sector_count)(const struct block_device *self);

    // Both return 1 on success, 0 on failure. `count` sectors from
    // `lba`, contiguous.
    int (*read_sectors)(const struct block_device *self, uint64_t lba, int count, void *buf);
    int (*write_sectors)(const struct block_device *self, uint64_t lba, int count, const void *buf);

    // Largest `count` a single transfer may use. A caller that ignores
    // this gets a refused transfer, not a short one.
    int (*max_sectors_per_xfer)(const struct block_device *self);

    // Does what is written here SURVIVE A POWER CYCLE? A disk does; a
    // live image in RAM does not. This is where the answer belongs --
    // a filesystem backend cannot tell, it just reads and writes
    // sectors, and fs_is_persistent() reporting "yes" for a live
    // session would be the single most misleading thing this feature
    // could do (see docs/live-cd-design.md).
    int persistent;

    unsigned caps;      // BLK_CAP_*
    int (*flush)(const struct block_device *self);   // BLK_CAP_FLUSH, 1 = durable
    int (*trim)(const struct block_device *self, uint64_t lba, uint32_t count); // BLK_CAP_TRIM
    // OPTIONAL: many runs in as few commands as the device allows. NULL
    // is fine -- blkdev_trim_ranges() then calls trim() once per run.
    int (*trim_ranges)(const struct block_device *self, const struct blk_range *r, int n);
    // OPTIONAL: several independent transfers IN FLIGHT AT ONCE, each
    // reporting its own result in `io[i].ok`. Returns 1 only if every one
    // succeeded. NULL is fine -- blkdev_submit_batch() then issues them
    // one at a time. A device with a queue (AHCI's NCQ) is what makes it
    // worth having: the per-command latency overlaps instead of adding.
    int (*submit_batch)(const struct block_device *self, struct blk_io *io, int n);

    // THE DEVICE'S LOGICAL BLOCK, in bytes: 512 or 4096. 0 means 512.
    //
    // EVERY LBA AND COUNT IN THIS LAYER STAYS IN 512-BYTE UNITS -- Linux's
    // `sector_t` rule. A 4K-sector disk is addressed as eight sectors per
    // block, the driver converts, and the block layer REFUSES a transfer
    // that does not start and end on a block boundary (Linux fails a
    // misaligned bio the same way). A one-sector metadata read into a
    // 512-byte buffer therefore fails loudly on such a disk instead of
    // overflowing -- use blkdev_read_partial() for those.
    uint32_t block_size;
};

// ---- the device table ------------------------------------------------
//
// EVERY device a driver finds is registered here, whether or not it
// carries the root. That split -- enumerate everything, choose the root
// separately -- is Linux's and Windows NT's alike: a Linux driver
// registers `sda`/`vda`/`nvme0n1` as it probes and the root comes from
// `root=` on the command line, and NT's PnP manager builds a device
// object per device while the boot path comes from BCD's `osdevice`.
//
// toy-os had the two FUSED, and it cost exactly what you would expect.
// `if (!blk_virtio_init() && !blk_ahci_init()) blk_ata_init();` short-
// circuits, so a machine with a virtio disk never ran the AHCI driver at
// all and its SATA disk did not exist -- unmountable, invisible to
// `parttable`, absent from every tool. A live boot was worse: it
// registered the RAM image and skipped disk init entirely, so a live
// session could not see the machine's own disks, which is most of what
// a live CD is for.
//
// NAMES ARE `<driver><index>`, and a partition is `<disk>p<n>` --
// `ata0`, `ahci0`, `virtio0`, `ram0`, `ahci0p1`. Named after the DRIVER
// rather than Linux's sd/vd/nvme split because the driver is a real
// user-facing lever here (`novirtio` and `noahci` already exist) and
// because this OS has no /dev for `/dev/sda1` to be a path into.
#define BLK_NAME_MAX 16     // "virtio0p15" and room to spare
#define BLK_MAX_DEVICES 16  // disks plus their partitions

struct blk_entry {
    const struct block_device *dev;
    const struct block_device *parent; // the disk it sits on; itself for a disk
    uint64_t base_lba;                 // where it starts on that disk
    char name[BLK_NAME_MAX];
};

// Forgets a device's NAME, so a re-read of a disk's partition table can
// hand the same name to a different window. Returns 0 for a device that
// is not there, or that is the ACTIVE device or the disk under it --
// forgetting either would leave the running volume unnameable.
//
// THE CALLER OWNS THE HARDER QUESTION. This cannot see the mount table,
// so it cannot know whether something is mounted from `dev`; releasing a
// window a mount still points at would leave that mount holding a slot
// somebody else can reuse. mount.c's rescan is the one caller and it
// checks.
int blk_untrack(const struct block_device *dev);

// How many devices are registered, and the i'th of them. NULL past the
// end, so a caller can walk without asking the count first.
int blk_device_count(void);
const struct blk_entry *blk_device_at(int i);

// The entry for a name (`ahci0`, `ahci0p1`), or NULL. Exact match.
const struct blk_entry *blk_device_by_name(const char *name);

// This device's registered name, or "?" for one that is not in the
// table. Never NULL -- a diagnostic must not have to check.
const char *blk_device_name(const struct block_device *dev);

// Adds a device to the table WITHOUT touching the root. For a device
// that is created but never made active -- a partition somebody mounts
// at /boot, which reaches blk_part_create() and not blk_register().
// Idempotent. Returns 0 only if the table is full.
//
// The distinction matters because the table is what NAMES a device, and
// a device with no name cannot be reached: `mount ata0p2 /mnt` failed
// for exactly this reason while /boot was mounted from that very
// partition.
int blk_track(const struct block_device *dev,
              const struct block_device *parent, uint64_t base_lba);

// Makes an already-registered device the ROOT: what blk_root() and the
// other blk_root_*() answer for. Returns 0 for a device that is not
// in the table, which is what stops `root=` naming something that was
// never found.
//
// SEPARATE FROM REGISTRATION on purpose -- that is the whole point of
// the table. A driver finding a disk no longer thereby claims the root.
int blk_set_root(const struct block_device *dev);

// Registers a device. Returns 0 (and registers nothing) if the device's
// capability bits and function pointers disagree.
//
// STILL SETS THE ROOT, last-writer-wins, so the boot order in
// kernel/fs/mount.c keeps deciding precedence exactly as it did. What
// changed is that losing that race no longer means being forgotten.
int blk_register(const struct block_device *dev);

// Registers `dev` as the root while recording what it SITS ON. A plain
// disk is its own whole disk at base 0, which is what blk_register()
// passes; a partition passes its parent and its start LBA.
//
// One entry point rather than a register-then-annotate pair, because a
// second call is a thing to forget, and forgetting it would leave the
// partition table being read out of the mounted partition instead of
// off the disk -- a wrong answer, not an error.
int blk_register_over(const struct block_device *dev,
                      const struct block_device *parent, uint64_t base_lba);

// THE ROOT DEVICE -- the volume the root filesystem is mounted from, or
// NULL when nothing is registered, which is the normal state on a
// machine with no disk and no live image and is what "RAM-only, nothing
// mounted" means. It is a NAME for one device, not an I/O path: a
// caller doing I/O passes the device to blkdev_*(), which is what every
// mount does since mount points became real.
const struct block_device *blk_root(void);
int blk_root_present(void);
int blk_root_persistent(void);
const char *blk_root_name(void);       // the driver's stem, "none" with no root

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

// Registers the AHCI drive as the active device, if there is one and
// it should have it. Returns 1 if it took the role.
//
// PRECEDENCE: virtio-blk first (measured, see block_virtio.c), then
// AHCI, then ATA -- AHCI is what a modern machine presents and legacy
// IDE is the fallback. `noahci` on the boot line forces ATA back, which
// is what keeps that path reachable and therefore tested. The choice is
// made in one place, kernel/fs/mount.c.
int blk_ahci_init(void);

// Registers every NVMe namespace the driver found: the first as the
// active device, the rest named but not active. `nonvme` on the boot
// line registers none. PRECEDENCE: after AHCI and before virtio, so an
// NVMe drive beats SATA on real hardware and virtio still wins in a VM.
int blk_nvme_init(void);

// Registers a RAM-backed device over [base, base + bytes). For a live
// image handed over by the bootloader; see kernel/drivers/block/block_ram.c.
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
// THE ACTIVE DEVICE IS NO LONGER THE ONLY ONE. It stays singular --
// it is what `parttable`, `mkpart` and the ROOT filesystem mean by
// "the disk" -- but Real mount points needs two volumes alive at once,
// so creating a partition device is now separate from making it
// active. blk_part_create() hands one back; blk_part_register() does
// that and then makes it the active device, which is what the boot
// scan and `fsformat` want.
//
// `parent` must be a device that is already known-good (in practice
// blk_root() immediately after a disk driver registered), and it is
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
                      uint64_t base_lba, uint64_t sectors, int index);

// The same window WITHOUT making it the root -- what a second mount
// needs, since the mount table holds a device per mount and only one
// of them can be blk_root(). Asking twice for the same window
// returns the SAME device, so pointer identity answers "is this volume
// already mounted?"; the pool is small and bounded, and a caller that
// exhausts it gets NULL and a logged reason.
const struct block_device *blk_part_create(const struct block_device *parent,
                                           uint64_t base_lba, uint64_t sectors,
                                           int index);

// Releases a partition window: frees its slot and forgets its name, so
// the next scan of that disk can hand the name to a different window.
// Returns 0 for a device that is not a window, or that blk_untrack()
// refused. Same caller obligation as blk_untrack().
int blk_part_release(const struct block_device *dev);

// If `dev` is a partition window, the device it sits on and where it
// starts there; NULL if it is not one. Lets a caller holding any
// device answer "which disk is this really on" without the active
// device having to be it.
const struct block_device *blk_part_parent(const struct block_device *dev,
                                           uint64_t *out_base);

// The disk the root sits on: the root itself, or its PARENT when the
// root is a partition. What `parttable`, `mkpart` and `fsformat` mean by
// "the disk" when not told another. NULL when nothing is registered.
const struct block_device *blk_root_disk(void);

// ---- I/O on a NAMED device ------------------------------------------
//
// The blk_* wrappers above all mean "the active device", which is the
// right default for the root filesystem and wrong for every other
// mount. A backend that was handed its own device at mount time uses
// these instead: same fault-injection hooks, same NULL tolerance, no
// dependence on which mount happens to be active.
//
// A backend calling the active-device wrappers while mounted somewhere
// else reads the WRONG VOLUME and reports no error, so this pair is
// not a convenience -- it is the whole reason fs_ops.init() takes a
// device.
int blkdev_read_sectors(const struct block_device *dev, uint64_t lba, int count, void *buf);
int blkdev_write_sectors(const struct block_device *dev, uint64_t lba, int count, const void *buf);
int blkdev_max_sectors_per_xfer(const struct block_device *dev);
int blkdev_flush(const struct block_device *dev);
int blkdev_trim_supported(const struct block_device *dev);
int blkdev_trim(const struct block_device *dev, uint64_t lba, uint32_t count);

// Linux's plug: hand the device several transfers TOGETHER so it can
// keep them all in flight, and wait for all of them. Returns 1 only if
// every transfer succeeded; each also reports in io[i].ok, so a caller
// can say which one failed. Waits either way -- this is a batch, not an
// asynchronous submit.
int blkdev_submit_batch(const struct block_device *dev, struct blk_io *io, int n);
// Every run in one go: a filesystem freeing a big file hands over its
// whole list rather than paying a command per run (a 512 MiB delete was
// 313 TRIMs and 89 ms). 1 if every run was discarded.
int blkdev_trim_ranges(const struct block_device *dev, const struct blk_range *r, int n);
uint64_t blkdev_sector_count(const struct block_device *dev);

// ---- logical block size ---------------------------------------------
//
// Bytes per logical block (512 when the device leaves it 0, or for NULL),
// and the same in 512-byte sectors -- 1 or 8. A transfer on `dev` must
// start and end on a multiple of blkdev_block_sectors(); one that does
// not is refused and logged.
uint32_t blkdev_block_size(const struct block_device *dev);
uint32_t blkdev_block_sectors(const struct block_device *dev);

// `count` sectors from `lba` WHATEVER THE ALIGNMENT, through a bounce
// buffer covering the enclosing blocks; the aligned case goes straight
// through. For small metadata (a superblock, a partition-table header),
// never for bulk data. At most one transfer's worth.
//
// THE WRITE IS A READ-MODIFY-WRITE OF THE WHOLE BLOCK, so it rewrites
// the neighbouring sectors with what was read a moment earlier: two
// partial writes to one block that race lose one of them, and a torn
// write can damage the neighbours. A caller shares a block only with
// data it holds the lock for.
int blkdev_read_partial(const struct block_device *dev, uint64_t lba, int count, void *buf);
int blkdev_write_partial(const struct block_device *dev, uint64_t lba, int count, const void *buf);

// ATA DATA SET MANAGEMENT's payload: 512-byte blocks of 64 eight-byte
// entries, each a 48-bit LBA and a 16-bit count. Fills ONE block from
// `r`, resuming at run *ri, sector *done within it, and returns how many
// entries it wrote (0 once the list is spent). Shared by ata.c and
// ahci.c, whose loops were identical.
#define BLK_DSM_ENTRIES   64
#define BLK_DSM_MAX_RANGE 0xFFFFu
int blk_dsm_pack(uint8_t *block, const struct blk_range *r, int n, int *ri, uint32_t *done);

#endif
