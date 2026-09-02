#ifndef KERNEL_VIRTIO_NET_H
#define KERNEL_VIRTIO_NET_H

#include <stdint.h>

// virtio-net, the virtqueue side. Knows about descriptors, the 12-byte
// virtio header and the two queues; knows NOTHING about net_device --
// the adapter (kernel/drivers/net/net_virtio.c) is what makes it one,
// exactly as block_virtio.c does for the disk.

struct pci_device;
void virtio_net_attach(const struct pci_device *pci);
int  virtio_net_present(void);
const uint8_t *virtio_net_mac(void);

// One complete Ethernet frame, no virtio header (this adds it) and no
// FCS. 0, or -ENOSPC when every transmit buffer is still in flight.
int virtio_net_transmit(const void *frame, uint32_t len);

// Hand back every received frame, then refill the queue. Called from
// the interrupt handler, or from the device's poll op when the line is
// unusable.
void virtio_net_drain(void);

// Where drained frames go. Set by the adapter before the device is
// registered; without it frames are counted and discarded.
void virtio_net_set_rx(void (*sink)(const void *frame, uint32_t len));

#endif
