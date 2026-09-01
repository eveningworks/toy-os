// The virtio disk, as a block device. A thin adapter, the same shape as
// block_ata.c: the driver keeps its behaviour and this only states
// which of it the block layer may use, under which capability bit.
#include "block.h"
#include "virtio_blk.h"
#include "klog.h"
#include "kfmt.h"
#include "ata.h"
#include "multiboot.h"
#include "string.h"
#include "driver.h" // DRIVER_REGISTER -- `lsdrv`

static uint32_t vblk_sector_count(void) { return virtio_blk_sector_count(); }
static int vblk_read(uint32_t lba, int count, void *buf) { return virtio_blk_read_sectors(lba, count, buf); }
static int vblk_write(uint32_t lba, int count, const void *buf) { return virtio_blk_write_sectors(lba, count, buf); }
static int vblk_max_xfer(void) { return virtio_blk_max_sectors_per_xfer(); }
static int vblk_flush(void) { return virtio_blk_flush(); }
static int vblk_trim(uint32_t lba, uint32_t count) { return virtio_blk_discard(lba, count); }

// FLUSH is decided at registration from the NEGOTIATED features, which
// is a deliberate divergence from block_ata.c's advertise-uncondition-
// ally rule -- and the divergence is the point rather than an
// inconsistency. ATA sets its bit blind because identify data is not
// necessarily settled when the adapter registers; virtio feature
// negotiation has already completed by then and definitively answers
// "can this device flush". So here the bit can mean exactly that.
//
// Mutable, and STATIC rather than a local: blk_register() keeps the
// POINTER, so a stack copy would leave the block layer reading a dead
// frame. That is the trap this registry invites.
static struct block_device VIRTIO_DEV = {
    .name = "virtio-blk",
    .sector_count = vblk_sector_count,
    .read_sectors = vblk_read,
    .write_sectors = vblk_write,
    .max_sectors_per_xfer = vblk_max_xfer,
    .persistent = 1,
    .caps = 0,
    .flush = 0,
    .trim = 0,
};

// Did the boot line ask to stay on ATA? Matched as a whole word, so
// `novirtio` does not also match a longer flag. Same shape as
// `nokaslr`/`nopat`/`notsc`, and it exists for the same reason those
// do: a fallback nothing can reach is a guess, and ATA is still the
// only disk on real hardware.
static int virtio_disabled(void) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "novirtio")) != 0; p += 8) {
        if (p != cmdline && p[-1] != ' ') continue;
        char after = p[8];
        if (after == 0 || after == ' ') return 1;
    }
    return 0;
}

int blk_virtio_init(void) {
    // DECLARED BEFORE THE HARDWARE IS LOOKED FOR, so a driver
    // that finds nothing still appears in `lsdrv` -- "compiled
    // in but idle" is the answer somebody is looking for.
    DRIVER_REGISTER("virtio-blk", "block");
    if (!virtio_blk_present()) return 0;

    // VIRTIO-BLK IS THE PREFERRED DISK when one is attached; ATA is the
    // fallback and the legacy path. That is the opposite of the rule
    // this shipped with, and the reason is measured rather than
    // aesthetic: ~10x ATA's write throughput under KVM (docs/testing.md
    // has the table), and the kernel test suite runs 6.4 s on virtio
    // against 11.9 s on ATA, passing 3 runs in 3 where ATA passed 2 in
    // 3. ATA's remaining flake is a host-I/O stall its ~1 s pre-issue
    // budget cannot absorb, and a virtqueue simply is not exposed to
    // it.
    //
    // A machine with only an IDE disk is unaffected: virtio_blk_present()
    // is 0 there and this returns immediately.
    if (virtio_disabled()) return 0;

    if (virtio_blk_flush_supported()) {
        VIRTIO_DEV.caps |= BLK_CAP_FLUSH;
        VIRTIO_DEV.flush = vblk_flush;
    }
    // TRIM comes from the device's own maximum, not from the feature
    // bit: a device may negotiate DISCARD and advertise a zero
    // max_discard_sectors, which means it cannot take one. QEMU does
    // that unless the drive was given `discard=unmap`, so this bit is
    // absent on a plainly-attached disk and present on the Makefile's.
    if (virtio_blk_discard_supported()) {
        VIRTIO_DEV.caps |= BLK_CAP_TRIM;
        VIRTIO_DEV.trim = vblk_trim;
    }

    // No announcement here: blk_register() already logs
    // "block: <name> active (<n> sectors)" for every device it accepts,
    // and block_ata.c stays quiet for the same reason. A line here made
    // the boot log report virtio-blk twice, which reads as two disks.
    driver_bound("virtio-blk", VIRTIO_DEV.name);
    return blk_register(&VIRTIO_DEV);
}
