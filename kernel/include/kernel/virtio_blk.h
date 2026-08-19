#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

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
void virtio_blk_init(void);

// Is there a working virtio-blk device? 0 when none was found, or when
// one was found and refused.
int virtio_blk_present(void);

// Capacity in 512-byte sectors, clamped to 32 bits -- see the block
// layer's own sector_count(), which is uint32_t.
uint32_t virtio_blk_sector_count(void);

int virtio_blk_read_sectors(uint32_t lba, int count, void *buf);
int virtio_blk_write_sectors(uint32_t lba, int count, const void *buf);
int virtio_blk_flush(void);
int virtio_blk_max_sectors_per_xfer(void);
int virtio_blk_flush_supported(void);

#endif
