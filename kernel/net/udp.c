// UDP: eight bytes of header, and a port to demultiplex on.
//
// THE CHECKSUM COVERS A HEADER THAT IS NOT ON THE WIRE. UDP's checksum
// includes a PSEUDO-HEADER -- source address, destination address,
// protocol and UDP length -- which exists nowhere in the packet and is
// reconstructed by both ends. That is what makes a datagram delivered
// to the wrong host detectable, and it is the one part of UDP that is
// easy to get wrong in a way that still works locally: a stack that
// omits it agrees with itself perfectly and is rejected by everything
// else. `tools/net_test.py` recomputes it on the host for that reason.
//
// A ZERO CHECKSUM MEANS "NOT COMPUTED" and must be accepted, which is
// why verification is skipped rather than failed on that value. It is
// legal in IPv4 UDP and illegal in IPv6, and QEMU's SLIRP sends zero.
// Transmitting zero is not done here: the wire value 0xFFFF is how a
// genuine all-ones sum is written so it cannot be mistaken for it.
#include "net.h"
#include "netdev.h"
#include "string.h"
#include "errno.h"

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;     // header + payload
    uint16_t checksum;   // 0 means not computed
} __attribute__((packed));

_Static_assert(sizeof(struct udp_header) == 8, "UDP header is 8 bytes on the wire");

static uint8_t g_out[sizeof(struct udp_header) + NET_UDP_MAX];

// The pseudo-header, summed into the running total before the datagram
// itself. Laid out as the twelve bytes RFC 768 describes rather than
// summed field by field, so it reads like the spec it implements.
static uint16_t udp_checksum(uint32_t src, uint32_t dst,
                             const uint8_t *datagram, uint32_t len) {
    uint8_t pseudo[12];
    pseudo[0] = (uint8_t)(src >> 24); pseudo[1] = (uint8_t)(src >> 16);
    pseudo[2] = (uint8_t)(src >> 8);  pseudo[3] = (uint8_t)src;
    pseudo[4] = (uint8_t)(dst >> 24); pseudo[5] = (uint8_t)(dst >> 16);
    pseudo[6] = (uint8_t)(dst >> 8);  pseudo[7] = (uint8_t)dst;
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_UDP;
    pseudo[10] = (uint8_t)(len >> 8);
    pseudo[11] = (uint8_t)len;
    return net_checksum_two(pseudo, sizeof pseudo, datagram, len);
}

int udp_input(struct net_device *dev, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t *pkt, uint32_t len) {
    (void)dst_ip;
    if (len < sizeof(struct udp_header)) return 1;   // malformed: dropped, not reported

    struct udp_header h;
    k_memcpy(&h, pkt, sizeof h);

    uint32_t total = net_ntohs(h.length);
    if (total < sizeof h || total > len) return 1;  // trailing padding is fine; a lie is not

    // A datagram whose checksum is wrong is DROPPED and not reported:
    // the port it names cannot be trusted, so a report would be sent
    // about a port nobody asked for, at an address that may also be
    // corrupt.
    if (h.checksum && udp_checksum(src_ip, dst_ip, pkt, total) != 0) return 1;

    uint16_t sport = net_ntohs(h.src_port);
    uint16_t dport = net_ntohs(h.dst_port);
    const uint8_t *payload = pkt + sizeof h;
    uint32_t plen = total - (uint32_t)sizeof h;

    return net_sock_deliver_udp(dev, src_ip, sport, dport, payload, plen);
}

int udp_output(struct net_device *dev, uint32_t dst_ip, uint16_t dst_port,
               uint16_t src_port, const void *payload, uint32_t len) {
    if (len > NET_UDP_MAX) return -EINVAL;

    uint32_t next_hop = 0;
    struct net_device *out = dev ? dev : ipv4_route(dst_ip, &next_hop);
    if (!out) return -ENODEV;

    uint32_t total = (uint32_t)sizeof(struct udp_header) + len;
    struct udp_header *h = (struct udp_header *)g_out;
    h->src_port = net_htons(src_port);
    h->dst_port = net_htons(dst_port);
    h->length = net_htons((uint16_t)total);
    h->checksum = 0;
    if (len) k_memcpy(g_out + sizeof *h, payload, len);

    uint16_t sum = udp_checksum(out->ip, dst_ip, g_out, total);
    // An all-ones sum is written as 0xFFFF, because 0 on the wire means
    // "no checksum" -- the one value the field cannot carry.
    h->checksum = net_htons(sum ? sum : 0xFFFF);

    return ipv4_output(dev, dst_ip, IP_PROTO_UDP, g_out, total);
}
