// IPv4: the 20-byte header, the checksum, and one routing decision.
//
// NO FRAGMENTATION, IN EITHER DIRECTION. A datagram arriving with MF
// set or a non-zero offset is DROPPED rather than reassembled, and
// nothing here ever emits one -- every sender above is capped at the
// device MTU. That is a real limitation and it is stated where it
// happens: reassembly needs a timer, a hole list and a memory budget
// that an attacker chooses, which is a subsystem rather than a branch.
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

#define IP_FLAG_MF     0x2000
#define IP_FRAG_MASK   0x1FFF
#define IP_DEFAULT_TTL 64

static uint8_t g_datagram[NET_MTU];
static uint16_t g_next_id = 1;

uint16_t net_checksum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);          // odd trailing byte, high half
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

struct net_device *ipv4_route(uint32_t dst, uint32_t *out_next_hop) {
    if (!dst) return 0;
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

    uint16_t frag = net_ntohs(h.flags_frag);
    if ((frag & IP_FLAG_MF) || (frag & IP_FRAG_MASK)) return;  // see the file comment

    uint32_t dst = net_ntohl(h.dst);
    if (dst != dev->ip && dst != 0xFFFFFFFFu) return;

    uint32_t src = net_ntohl(h.src);
    const uint8_t *payload = pkt + ihl;
    uint32_t plen = total - ihl;

    if (h.proto == IP_PROTO_ICMP) icmp_input(dev, src, payload, plen);
}

int ipv4_output(struct net_device *dev, uint32_t dst_ip, uint8_t proto,
                const void *payload, uint32_t len) {
    uint32_t next_hop = dst_ip;
    if (!dev) dev = ipv4_route(dst_ip, &next_hop);
    else if ((dev->ip & dev->netmask) != (dst_ip & dev->netmask)) next_hop = dev->gateway;

    if (!dev || !dev->ip) return -ENODEV;
    if (!next_hop) return -ENODEV;
    if (len + sizeof(struct ipv4_header) > dev->mtu) return -EINVAL;

    uint8_t mac[NET_MAC_LEN];
    if (!arp_resolve(dev, next_hop, mac)) return -EAGAIN;  // request sent; retry

    struct ipv4_header *h = (struct ipv4_header *)g_datagram;
    h->version_ihl = 0x45;
    h->dscp_ecn = 0;
    h->total_len = net_htons((uint16_t)(sizeof *h + len));
    h->id = net_htons(g_next_id++);
    h->flags_frag = 0;
    h->ttl = IP_DEFAULT_TTL;
    h->proto = proto;
    h->checksum = 0;
    h->src = net_htonl(dev->ip);
    h->dst = net_htonl(dst_ip);
    h->checksum = net_htons(net_checksum(h, sizeof *h));
    k_memcpy(g_datagram + sizeof *h, payload, len);

    return eth_output(dev, mac, ETH_TYPE_IPV4, g_datagram, sizeof *h + len);
}
