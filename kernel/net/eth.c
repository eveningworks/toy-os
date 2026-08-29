// Ethernet: the 14-byte header, and which protocol gets the payload.
//
// The frame this builds is DESTINATION, SOURCE, ETHERTYPE, payload --
// no VLAN tag, no 802.3 length interpretation (a value below 0x0600 in
// that field is a length rather than a type; nothing here speaks it and
// such a frame is dropped), and no FCS, which every NIC appends itself.
#include "net.h"
#include "netdev.h"
#include "string.h"
#include "errno.h"

const uint8_t ETH_BROADCAST[NET_MAC_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// One frame under construction. Safe as a static because nothing above
// the driver runs from an interrupt (net.c's queue is what guarantees
// that) and this kernel does not preempt inside a transmit.
static uint8_t g_frame[NET_FRAME_MAX];

void eth_input(struct net_device *dev, const uint8_t *frame, uint32_t len) {
    if (len < ETH_HDR_LEN) return;

    // Accept broadcast and our own address only. The NIC's own filter
    // normally does this, but an emulated device can be looser, and a
    // stack that answers ARP for somebody else's MAC is a bug that only
    // shows up on a busy network.
    const uint8_t *dst = frame;
    if (dst[0] & 1) {
        // Multicast/broadcast bit. Only broadcast is understood.
        if (k_memcmp(dst, ETH_BROADCAST, NET_MAC_LEN) != 0) return;
    } else if (k_memcmp(dst, dev->mac, NET_MAC_LEN) != 0) {
        return;
    }

    uint16_t type = net_ntohs(*(const uint16_t *)(frame + 12));
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint32_t plen = len - ETH_HDR_LEN;

    switch (type) {
    case ETH_TYPE_ARP:  arp_input(dev, payload, plen);  break;
    case ETH_TYPE_IPV4: ipv4_input(dev, payload, plen); break;
    default: break;  // IPv6, LLDP, an 802.3 length -- not ours
    }
}

int eth_output(struct net_device *dev, const uint8_t dst_mac[NET_MAC_LEN],
               uint16_t ethertype, const void *payload, uint32_t len) {
    if (!dev || !dst_mac || (len && !payload)) return -EINVAL;
    if (len > dev->mtu) return -EINVAL;

    k_memcpy(g_frame, dst_mac, NET_MAC_LEN);
    k_memcpy(g_frame + 6, dev->mac, NET_MAC_LEN);
    *(uint16_t *)(g_frame + 12) = net_htons(ethertype);
    k_memcpy(g_frame + ETH_HDR_LEN, payload, len);

    // Pad to the 60-byte minimum (64 with the FCS the NIC adds). An
    // ARP request is 42 bytes and a receiver is entitled to discard a
    // runt; some emulated NICs pad for you and some do not.
    uint32_t total = ETH_HDR_LEN + len;
    if (total < 60) {
        k_memset(g_frame + total, 0, 60 - total);
        total = 60;
    }
    return net_tx(dev, g_frame, total);
}
