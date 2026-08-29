// The virtio NIC, as a net_device. A thin adapter, the same shape as
// block_virtio.c: the driver keeps its behaviour and this only states
// which of it the network core may use.
#include "netdev.h"
#include "virtio_net.h"
#include "string.h"

static struct net_device VIRTIO_NET_DEV = {
    .driver = "virtio-net",
};

static int vnet_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev;
    return virtio_net_transmit(frame, len);
}

// The driver hands frames here with no device pointer, because it has
// never heard of one -- this is the only place that knows the two are
// the same card.
static void vnet_rx(const void *frame, uint32_t len) {
    net_rx(&VIRTIO_NET_DEV, frame, len);
}

void net_virtio_init(void) {
    virtio_net_init();
    if (!virtio_net_present()) return;   // no such device is the ordinary case

    k_memcpy(VIRTIO_NET_DEV.mac, virtio_net_mac(), NET_MAC_LEN);
    VIRTIO_NET_DEV.transmit = vnet_transmit;
    virtio_net_set_rx(vnet_rx);
    net_register(&VIRTIO_NET_DEV);
}
