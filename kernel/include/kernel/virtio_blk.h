#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

struct pci_device;

// The virtio block device. See kernel/drivers/virtio/virtio_blk.c.
//
// Brings the driver up if a virtio-blk device is on the PCI bus:
// claims it, negotiates features, sets up its one queue and reads the
// capacity. Silent and harmless when there is no such device, which is
// the ordinary case on every default boot.
//
// Called from kernel_main() after pci_init(), NOT from fs_init(): the
// driver existing is independent of whether anything mounts off it,
// and `dmesg` should say a virtio disk is present either way.

// Is there a working virtio-blk device? 0 when none was found, or when
// one was found and refused.
int virtio_blk_present(void);
const struct pci_device *virtio_blk_pci(void);

// Capacity in 512-byte sectors, clamped to 32 bits -- see the block
// layer's own sector_count(), which is uint32_t.
uint64_t virtio_blk_sector_count(void);

int virtio_blk_read_sectors(uint64_t lba, int count, void *buf);
int virtio_blk_write_sectors(uint64_t lba, int count, const void *buf);
int virtio_blk_flush(void);

// DISCARD -- virtio's TRIM. Tells the host `count` sectors from `lba`
// hold nothing worth keeping, which is what stops a sparse disk.img
// growing forever (sparseness is only ever lost).
//
// REFUSED, NEVER SHORT: a range larger than the device's own
// max_discard_sectors comes back 0 rather than being partly done, since
// a partial discard reporting success would leave the caller believing
// blocks were released that were not.
int virtio_blk_discard(uint64_t lba, uint32_t count);

// Whether a discard issued right now would go out. NOT the feature bit:
// a device may negotiate DISCARD and advertise a zero maximum, which
// means it cannot take one. QEMU does exactly that unless the drive was
// given `discard=unmap`.
int virtio_blk_discard_supported(void);
int virtio_blk_max_sectors_per_xfer(void);
// Bytes per LOGICAL block -- 512, or 4096 on a 4K-sector disk. Every
// sector number above stays in 512-byte units either way.
uint32_t virtio_blk_block_size(void);
int virtio_blk_flush_supported(void);

#endif
