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

static uint32_t vblk_sector_count(void) { return virtio_blk_sector_count(); }
static int vblk_read(uint32_t lba, int count, void *buf) { return virtio_blk_read_sectors(lba, count, buf); }
static int vblk_write(uint32_t lba, int count, const void *buf) { return virtio_blk_write_sectors(lba, count, buf); }
static int vblk_max_xfer(void) { return virtio_blk_max_sectors_per_xfer(); }
static int vblk_flush(void) { return virtio_blk_flush(); }

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

// Did the boot line ask for virtio-blk explicitly? Matched as a whole
// word, so `virtioblk` does not also match some longer flag.
static int virtioblk_requested(void) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "virtioblk")) != 0; p += 9) {
        if (p != cmdline && p[-1] != ' ') continue;
        char after = p[9];
        if (after == 0 || after == ' ') return 1;
    }
    return 0;
}

int blk_virtio_init(void) {
    if (!virtio_blk_present()) return 0;

    // ATA has a disk and nobody asked -- leave it alone. See block.h.
    if (ata_present() && !virtioblk_requested()) return 0;

    if (virtio_blk_flush_supported()) {
        VIRTIO_DEV.caps |= BLK_CAP_FLUSH;
        VIRTIO_DEV.flush = vblk_flush;
    }
    // No BLK_CAP_TRIM: VIRTIO_BLK_F_DISCARD needs discard=unmap on the
    // drive and a separate segment format, and TFS3's trim path is
    // already exercised through ATA. Declaring no bit and providing no
    // trim() passes blk_register()'s both-directions honesty check.

    // No announcement here: blk_register() already logs
    // "block: <name> active (<n> sectors)" for every device it accepts,
    // and block_ata.c stays quiet for the same reason. A line here made
    // the boot log report virtio-blk twice, which reads as two disks.
    return blk_register(&VIRTIO_DEV);
}
