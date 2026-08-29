// The network-device core: the table, the names, and the receive queue.
//
// Everything here is about keeping two things apart that a NIC driver
// naturally wants to mix: the INTERRUPT, which must be short and may
// not allocate, parse or touch the filesystem, and the STACK, which
// does all three. A driver hands frames to net_rx() and returns; the
// protocols run later from net_poll(). That is Linux's netif_rx/NAPI
// split, and this kernel needs it for a sharper reason than Linux
// does -- kmalloc here is not interrupt-safe and the filesystem is not
// re-entrant, so parsing a packet in an ISR is not merely rude.
//
// THE QUEUE IS STATIC AND COPIES. A pool of frame-sized slots in .bss,
// written by the producer (an ISR) and read by the consumer
// (net_poll()), single-core lock-free on the usual head/tail rule:
// only the producer moves tail, only the consumer moves head. Copying
// costs a memcpy per frame and buys the driver its DMA buffer back
// immediately, which is what stops a slow consumer stalling the ring.
#include "netdev.h"
#include "net.h"
#include "klog.h"
#include "kfmt.h"   // klog_printf
#include "string.h"
#include "errno.h"

#define NET_RX_QUEUE 32

static struct net_device *g_devs[NET_MAX_DEVS];
static int g_count;

struct rx_slot {
    struct net_device *dev;
    uint32_t len;
    uint8_t data[NET_FRAME_MAX];
};

static struct rx_slot g_rxq[NET_RX_QUEUE];
static volatile uint32_t g_rx_head;  // consumer
static volatile uint32_t g_rx_tail;  // producer (interrupt context)

int net_register(struct net_device *dev) {
    if (!dev || !dev->transmit) return 0;
    if (g_count >= NET_MAX_DEVS) {
        klog_printf("net: no room for another device (max %d)\n", NET_MAX_DEVS);
        return 0;
    }
    dev->name[0] = 'n'; dev->name[1] = 'e'; dev->name[2] = 't';
    dev->name[3] = (char)('0' + g_count);
    dev->name[4] = 0;
    if (!dev->mtu) dev->mtu = NET_MTU;

    g_devs[g_count++] = dev;
    klog_printf("net: %s: %s %02x:%02x:%02x:%02x:%02x:%02x mtu %u\n",
                dev->name, dev->driver ? dev->driver : "?",
                dev->mac[0], dev->mac[1], dev->mac[2],
                dev->mac[3], dev->mac[4], dev->mac[5], dev->mtu);
    return 1;
}

int net_device_count(void) { return g_count; }

struct net_device *net_device_at(int index) {
    if (index < 0 || index >= g_count) return 0;
    return g_devs[index];
}

struct net_device *net_device_by_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_count; i++)
        if (!k_strcmp(g_devs[i]->name, name)) return g_devs[i];
    return 0;
}

struct net_device *net_default_device(void) {
    for (int i = 0; i < g_count; i++)
        if (g_devs[i]->ip) return g_devs[i];
    return 0;
}

void net_rx(struct net_device *dev, const void *frame, uint32_t len) {
    if (!dev || !frame) return;
    if (len < ETH_HDR_LEN || len > NET_FRAME_MAX) { dev->rx_dropped++; return; }

    uint32_t tail = g_rx_tail;
    uint32_t next = (tail + 1) % NET_RX_QUEUE;
    if (next == g_rx_head) { dev->rx_dropped++; return; }  // full: drop the NEW one

    struct rx_slot *s = &g_rxq[tail];
    s->dev = dev;
    s->len = len;
    k_memcpy(s->data, frame, len);
    g_rx_tail = next;   // publish last: the consumer must not see a half-filled slot

    dev->rx_packets++;
    dev->rx_bytes += len;
}

int net_tx(struct net_device *dev, const void *frame, uint32_t len) {
    if (!dev || !frame) return -EINVAL;
    if (len < ETH_HDR_LEN || len > dev->mtu + ETH_HDR_LEN) return -EINVAL;

    int rc = dev->transmit(dev, frame, len);
    if (rc < 0) { dev->tx_dropped++; return rc; }
    dev->tx_packets++;
    dev->tx_bytes += len;
    return 0;
}

void net_poll(void) {
    // Re-entrancy guard: a ring-3 process is preemptible inside a
    // syscall, and both a socket call and scheduler_idle() reach here.
    // Two consumers on one head index would hand the same frame to the
    // stack twice. (ata_cache.c's idle hook has the same guard.)
    static int in_poll;
    if (in_poll) return;
    in_poll = 1;

    for (int i = 0; i < g_count; i++)
        if (g_devs[i]->poll) g_devs[i]->poll(g_devs[i]);

    while (g_rx_head != g_rx_tail) {
        struct rx_slot *s = &g_rxq[g_rx_head];
        eth_input(s->dev, s->data, s->len);
        g_rx_head = (g_rx_head + 1) % NET_RX_QUEUE;
    }

    in_poll = 0;
}

// QEMU's user-mode (SLIRP) network always hands out the same addresses:
// the guest is 10.0.2.15/24, the gateway and the DNS server are
// 10.0.2.2 and 10.0.2.3. Assigning them here is what makes `ping`
// work on a machine nobody configured -- a placeholder for DHCP, which
// is where an address is supposed to come from and is a roadmap item.
//
// ONLY THE FIRST device gets them, because two cards on one address is
// worse than one card with none: a second NIC is left unconfigured and
// `ifconfig` says so, rather than silently answering for an address
// that already belongs to something else.
void net_autoconfig(void) {
    for (int i = 0; i < g_count; i++) {
        if (g_devs[i]->ip) return;   // somebody has an address already
    }
    if (!g_count) return;
    g_devs[0]->ip      = NET_IPV4(10, 0, 2, 15);
    g_devs[0]->netmask = NET_IPV4(255, 255, 255, 0);
    g_devs[0]->gateway = NET_IPV4(10, 0, 2, 2);
    klog_printf("net: %s: 10.0.2.15/24 via 10.0.2.2 (QEMU user networking defaults)\n",
                g_devs[0]->name);
}

void net_init(void) {
    g_count = 0;
    g_rx_head = g_rx_tail = 0;
}
