// IPv4: the 20-byte header, the checksum, fragmentation, and one
// routing decision.
//
// FRAGMENTS ARE REASSEMBLED (ipv4_frag.c) AND AN OVER-MTU DATAGRAM IS
// SPLIT HERE, so a datagram up to IP_DATAGRAM_MAX travels either way.
// DF is never set and never honoured on the way out: setting it only
// pays with Path MTU Discovery behind it, which this stack does not do
// -- and TCP never needs to fragment, its MSS being MTU-sized.
//
// ROUTING IS TWO RULES, not a table: an address inside a device's own
// subnet goes straight to it, anything else goes to that device's
// gateway. Per-device configuration is what makes this multi-homed --
// the first device whose subnet contains the destination wins, and
// only then does the default fall through to a gateway.
#include "net.h"
#include "netdev.h"
#include "string.h"
#include "errno.h"
#include "clocksource.h"   // a fragment's arrival time

struct ipv4_header {
    uint8_t  version_ihl;     // 0x45 for a 20-byte header
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

_Static_assert(sizeof(struct ipv4_header) == 20, "IPv4 header is 20 bytes on the wire");

#define IP_DEFAULT_TTL 64

static uint8_t g_datagram[NET_MTU];
static uint16_t g_next_id = 1;

// The running sum, without the final complement, so two buffers can be
// summed as if concatenated. Splitting them is only correct when the
// first has an EVEN length -- an odd one would put the second buffer's
// first byte in the wrong half of a 16-bit word -- which is why the
// only caller passes a 12-byte pseudo-header.
static uint32_t sum16(const uint8_t *p, uint32_t len, uint32_t sum) {
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);          // odd trailing byte, high half
    return sum;
}

static uint16_t fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

uint16_t net_checksum(const void *data, uint32_t len) {
    return fold(sum16(data, len, 0));
}

uint16_t net_checksum_two(const void *a, uint32_t a_len,
                          const void *b, uint32_t b_len) {
    return fold(sum16(b, b_len, sum16(a, a_len, 0)));
}

struct net_device *ipv4_route(uint32_t dst, uint32_t *out_next_hop) {
    if (!dst) return 0;

    // A broadcast is not routed, it is put on a wire. The first
    // registered device is the only sensible default, and a caller that
    // means a particular card names it (udp_output's `dev`), which is
    // how a DHCP client reaches the segment it is asking on.
    if (dst == IP_BROADCAST) {
        if (out_next_hop) *out_next_hop = IP_BROADCAST;
        return net_device_at(0);
    }
    for (int i = 0; i < net_device_count(); i++) {
        struct net_device *d = net_device_at(i);
        if (!d->ip || !d->netmask) continue;
        if ((d->ip & d->netmask) == (dst & d->netmask)) {
            if (out_next_hop) *out_next_hop = dst;   // on-link
            return d;
        }
    }
    for (int i = 0; i < net_device_count(); i++) {
        struct net_device *d = net_device_at(i);
        if (!d->ip || !d->gateway) continue;
        if (out_next_hop) *out_next_hop = d->gateway;
        return d;
    }
    return 0;
}

void ipv4_input(struct net_device *dev, const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct ipv4_header)) return;

    struct ipv4_header h;
    k_memcpy(&h, pkt, sizeof h);

    if ((h.version_ihl >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(h.version_ihl & 0x0F) * 4;
    if (ihl < sizeof h || ihl > len) return;
    if (net_checksum(pkt, ihl) != 0) return;   // a correct header sums to zero

    uint32_t total = net_ntohs(h.total_len);
    if (total < ihl || total > len) return;    // trailing padding is fine; a lie is not

    uint32_t dst = net_ntohl(h.dst);
    // Our own address, or a broadcast. A device with NO address still
    // accepts broadcasts, which is the only way a DHCP offer can reach
    // the client that asked for one.
    if (dst != IP_BROADCAST && (!dev->ip || dst != dev->ip)) return;

    // Checked AFTER the address, so nobody else's fragments cost memory.
    uint16_t frag = net_ntohs(h.flags_frag);
    if ((frag & IP_FLAG_MF) || (frag & IP_FRAG_MASK)) {
        ipv4_frag_input(dev, pkt, ihl, total, clocksource_now_ns());
        return;
    }
    ipv4_deliver(dev, pkt, ihl, total);
}

void ipv4_deliver(struct net_device *dev, const uint8_t *pkt,
                  uint32_t ihl, uint32_t total) {
    struct ipv4_header h;
    k_memcpy(&h, pkt, sizeof h);
    uint32_t src = net_ntohl(h.src);
    uint32_t dst = net_ntohl(h.dst);
    const uint8_t *payload = pkt + ihl;
    uint32_t plen = total - ihl;

    if (h.proto == IP_PROTO_ICMP) {
        icmp_input(dev, src, payload, plen);
    } else if (h.proto == IP_PROTO_TCP) {
        // A segment for no connection is DROPPED rather than answered
        // with a RST. A RST needs the segment's own sequence numbers
        // reflected correctly, and getting that wrong is worse than
        // staying quiet: this stack makes only outbound connections, so
        // the only unmatched segments it sees are late ones from a
        // connection it has already forgotten.
        (void)tcp_input(dev, src, dst, payload, plen);
    } else if (h.proto == IP_PROTO_UDP) {
        if (udp_input(dev, src, dst, payload, plen)) return;
        // Nobody was listening. Say so rather than dropping in silence:
        // a client talking to the wrong port otherwise waits out its
        // whole timeout, which is the case where the diagnosis matters
        // most. NEVER for a broadcast -- answering one would have every
        // host on the segment reply to a datagram sent to all of them.
        if (dst != IP_BROADCAST && dst == dev->ip)
            icmp_send_error(dev, src, ICMP_DEST_UNREACHABLE, ICMP_CODE_PORT, pkt, total);
    }
}

int ipv4_output(struct net_device *dev, uint32_t dst_ip, uint8_t proto,
                const void *payload, uint32_t len) {
    uint32_t next_hop = dst_ip;
    if (!dev) dev = ipv4_route(dst_ip, &next_hop);
    else if (dst_ip != IP_BROADCAST &&
             (dev->ip & dev->netmask) != (dst_ip & dev->netmask)) next_hop = dev->gateway;

    if (!dev) return -ENODEV;
    // A BROADCAST IS THE ONE THING AN UNADDRESSED DEVICE MAY SEND, and
    // its source address is then 0.0.0.0 -- which is not a mistake but
    // the literal state a DHCP client is in until it has a lease.
    if (!dev->ip && dst_ip != IP_BROADCAST) return -ENODEV;
    if (!next_hop) return -ENODEV;
    if (len + sizeof(struct ipv4_header) > IP_DATAGRAM_MAX) return -EINVAL;
    if (dev->mtu <= sizeof(struct ipv4_header) + 8) return -EINVAL;

    uint8_t mac[NET_MAC_LEN];
    if (dst_ip == IP_BROADCAST) {
        k_memcpy(mac, ETH_BROADCAST, NET_MAC_LEN);   // nothing to resolve
    } else if (!arp_resolve(dev, next_hop, mac)) {
        return -EAGAIN;  // request sent; retry
    }

    // Every fragment but the last carries a whole number of 8-byte
    // blocks, which is the unit the offset field counts in. A datagram
    // that fits is the one-fragment case of the same loop.
    uint32_t chunk = (dev->mtu - (uint32_t)sizeof(struct ipv4_header)) & ~7u;
    uint16_t id = g_next_id++;
    const uint8_t *src = payload;
    uint32_t off = 0;
    do {
        uint32_t n = len - off < chunk ? len - off : chunk;
        int more = off + n < len;

        // Rebuilt whole for each fragment, so a sender preempted
        // BETWEEN fragments finds nothing of its own left in g_datagram.
        struct ipv4_header *h = (struct ipv4_header *)g_datagram;
        h->version_ihl = 0x45;
        h->dscp_ecn = 0;
        h->total_len = net_htons((uint16_t)(sizeof *h + n));
        h->id = net_htons(id);
        h->flags_frag = net_htons((uint16_t)((more ? IP_FLAG_MF : 0) | (off / 8)));
        h->ttl = IP_DEFAULT_TTL;
        h->proto = proto;
        h->checksum = 0;
        h->src = net_htonl(dev->ip);
        h->dst = net_htonl(dst_ip);
        h->checksum = net_htons(net_checksum(h, sizeof *h));
        if (n) k_memcpy(g_datagram + sizeof *h, src + off, n);

        int rc = eth_output(dev, mac, ETH_TYPE_IPV4, g_datagram, sizeof *h + n);
        if (rc < 0) return rc;
        off += n;
    } while (off < len);
    return 0;
}
