// KTESTs for the protocol stack, driven through the LIVE device.
//
// EVERY TEST HERE IS A PAIR: a frame that must be answered, beside one
// that must not. On its own "an ARP request got a reply" is satisfied
// by a stack that replies to everything, which is a worse bug than one
// that replies to nothing -- it answers for addresses that are not its.
// So each positive is followed by the same frame with one field
// changed, and the assertion is that the transmit counter did NOT move.
//
// FRAMES GO IN THROUGH eth_input(), the same entry net_poll() uses, so
// nothing here needs a network -- only a registered device to address.
// What it cannot cover is the drivers themselves; that is
// tools/net_test.py, which needs a host on the other end.
//
// THE GATE IS THE DEVICE TABLE, and it is honest about what it means: a
// machine with no NIC skips (a real state -- `NET=none`), and so does
// one whose card never got an address, because a stack with no address
// of its own is entitled to answer nothing.
#include "net.h"
#include "netdev.h"
#include "string.h"
#include "ktest.h"

// Frames live here rather than on the stack: the ring-0 frame budget is
// 1 KiB and these are close enough to it to be worth not testing.
static uint8_t g_frame[NET_FRAME_MAX];

static struct net_device *addressed_device(void) {
    for (int i = 0; i < net_device_count(); i++) {
        struct net_device *d = net_device_at(i);
        if (d && d->ip) return d;
    }
    return 0;
}

// The 14-byte header, addressed to `dev` unless `dst` says otherwise.
static uint32_t eth_frame(struct net_device *dev, const uint8_t *dst, uint16_t type) {
    static const uint8_t sender[NET_MAC_LEN] = { 0x52, 0x54, 0x00, 0xAA, 0xBB, 0xCC };
    k_memcpy(g_frame, dst ? dst : dev->mac, NET_MAC_LEN);
    k_memcpy(g_frame + 6, sender, NET_MAC_LEN);
    g_frame[12] = (uint8_t)(type >> 8);
    g_frame[13] = (uint8_t)type;
    return ETH_HDR_LEN;
}

// An ARP request from 10.99.99.99 asking who has `target`.
static uint32_t arp_request(struct net_device *dev, uint32_t target, const uint8_t *dst) {
    uint32_t n = eth_frame(dev, dst, ETH_TYPE_ARP);
    uint8_t *p = g_frame + n;
    p[0] = 0; p[1] = 1;                     // Ethernet
    p[2] = 0x08; p[3] = 0x00;               // IPv4
    p[4] = NET_MAC_LEN; p[5] = 4;
    p[6] = 0; p[7] = 1;                     // request
    k_memcpy(p + 8, g_frame + 6, NET_MAC_LEN);
    uint32_t sender_ip = NET_IPV4(10, 99, 99, 99);
    p[14] = (uint8_t)(sender_ip >> 24); p[15] = (uint8_t)(sender_ip >> 16);
    p[16] = (uint8_t)(sender_ip >> 8);  p[17] = (uint8_t)sender_ip;
    k_memset(p + 18, 0, NET_MAC_LEN);
    p[24] = (uint8_t)(target >> 24); p[25] = (uint8_t)(target >> 16);
    p[26] = (uint8_t)(target >> 8);  p[27] = (uint8_t)target;
    return n + 28;
}

// An IPv4 + ICMP echo request to `dst_ip`. `frag` goes into the flags/
// fragment field and `corrupt` breaks the header checksum, so one
// builder serves the positive case and both negatives.
static uint32_t icmp_echo(struct net_device *dev, uint32_t dst_ip,
                          uint16_t frag, int corrupt) {
    uint32_t n = eth_frame(dev, 0, ETH_TYPE_IPV4);
    uint8_t *ip = g_frame + n;
    const uint32_t payload = 16;
    const uint32_t icmp_len = 8 + payload;

    k_memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (uint8_t)((20 + icmp_len) >> 8); ip[3] = (uint8_t)(20 + icmp_len);
    ip[6] = (uint8_t)(frag >> 8); ip[7] = (uint8_t)frag;
    ip[8] = 64;
    ip[9] = IP_PROTO_ICMP;
    uint32_t src = NET_IPV4(10, 99, 99, 99);
    ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);  ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dst_ip >> 24); ip[17] = (uint8_t)(dst_ip >> 16);
    ip[18] = (uint8_t)(dst_ip >> 8);  ip[19] = (uint8_t)dst_ip;
    uint16_t sum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
    if (corrupt) ip[11] ^= 0xFF;

    uint8_t *icmp = ip + 20;
    k_memset(icmp, 0, icmp_len);
    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[4] = 0x12; icmp[5] = 0x34;   // identifier
    icmp[6] = 0x00; icmp[7] = 0x01;   // sequence
    for (uint32_t i = 0; i < payload; i++) icmp[8 + i] = (uint8_t)i;
    uint16_t csum = net_checksum(icmp, icmp_len);
    icmp[2] = (uint8_t)(csum >> 8); icmp[3] = (uint8_t)csum;

    return n + 20 + icmp_len;
}

KTEST("net", "a correct header sums to zero and a corrupted one does not") {
    // A real IPv4 header, checksum field included: the one's-complement
    // sum of a valid header is 0, which is what makes verification a
    // single call rather than a compare against a recomputed value.
    static const uint8_t hdr[20] = {
        0x45, 0x00, 0x00, 0x54, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x01, 0x22, 0x99, 0x0A, 0x00, 0x02, 0x0F,
        0x0A, 0x00, 0x02, 0x02,
    };
    KTEST_ASSERT_EQ(net_checksum(hdr, sizeof hdr), 0);

    uint8_t bad[20];
    k_memcpy(bad, hdr, sizeof hdr);
    bad[16] ^= 0x01;   // one bit of the destination address
    KTEST_ASSERT(net_checksum(bad, sizeof bad) != 0);
}

KTEST("net", "byte order helpers swap, and swap back") {
    KTEST_ASSERT_EQ(net_htons(0x1234), 0x3412);
    KTEST_ASSERT_EQ(net_htonl(0x12345678u), 0x78563412u);
    KTEST_ASSERT_EQ(net_ntohs(net_htons(0xBEEF)), 0xBEEF);
    KTEST_ASSERT_EQ(net_ntohl(net_htonl(0xDEADBEEFu)), 0xDEADBEEFu);
}

KTEST("net", "an ARP request for our address is answered, and one for another is not") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    uint64_t before = dev->tx_packets;
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before + 1);

    // The same request for an address that is not ours. A stack that
    // answers this claims somebody else's address on the wire.
    before = dev->tx_packets;
    len = arp_request(dev, NET_IPV4(10, 99, 99, 98), 0);
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before);
}

KTEST("net", "a frame addressed to another MAC is ignored") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    static const uint8_t other[NET_MAC_LEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    uint64_t before = dev->tx_packets;
    uint32_t len = arp_request(dev, dev->ip, other);
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before);
}

KTEST("net", "an echo request is answered") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    // The sender must be in the ARP cache, or the reply is deferred
    // behind a resolution nothing here will answer -- a precondition
    // this test establishes rather than inherits.
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    uint64_t before = dev->tx_packets;
    len = icmp_echo(dev, dev->ip, 0, 0);
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before + 1);
}

KTEST("net", "a fragment, a bad checksum and somebody else's address are all dropped") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);   // same precondition as above

    // More-fragments set. Reassembly is deliberately not implemented
    // (kernel/net/ipv4.c), so this must be dropped rather than treated
    // as a whole datagram that happens to parse.
    uint64_t before = dev->tx_packets;
    len = icmp_echo(dev, dev->ip, 0x2000, 0);
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before);

    // A non-zero fragment offset, which is the other half of the same
    // rule and would pass a check that only looked at the MF bit.
    len = icmp_echo(dev, dev->ip, 0x0001, 0);
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before);

    len = icmp_echo(dev, dev->ip, 0, 1);   // corrupted header checksum
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before);

    len = icmp_echo(dev, NET_IPV4(10, 99, 99, 98), 0, 0);  // not our address
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(dev->tx_packets, before);
}

KTEST("net", "routing picks an on-link device over a gateway") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    // An address inside the device's own subnet is delivered directly:
    // the next hop is the destination itself.
    uint32_t on_link = (dev->ip & dev->netmask) | 5;
    uint32_t next = 0;
    struct net_device *out = ipv4_route(on_link, &next);
    KTEST_ASSERT(out != 0);
    KTEST_ASSERT_EQ(next, on_link);

    // Anything else goes via that device's gateway -- and the next hop
    // being the GATEWAY rather than the destination is the whole
    // difference between the two rules.
    if (dev->gateway) {
        next = 0;
        out = ipv4_route(NET_IPV4(8, 8, 8, 8), &next);
        KTEST_ASSERT(out != 0);
        KTEST_ASSERT_EQ(next, out->gateway);
    }
}

KTEST("net", "a socket refuses everything but the one supported protocol") {
    KTEST_ASSERT(net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, 99) < 0);   // not ICMP
    KTEST_ASSERT(net_sock_open(99, NET_SOCK_DGRAM, IP_PROTO_ICMP) < 0); // not AF_INET
    KTEST_ASSERT(net_sock_open(NET_AF_INET, 99, IP_PROTO_ICMP) < 0);    // not SOCK_DGRAM

    int s = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_ICMP);
    KTEST_ASSERT(s >= 0);
    // An idle socket reports nothing rather than an error, which is
    // what every caller polls on.
    KTEST_ASSERT_EQ(net_sock_recvfrom(s, g_frame, sizeof g_frame, 0), 0);
    net_sock_close(s);
    KTEST_ASSERT(net_sock_recvfrom(s, g_frame, sizeof g_frame, 0) < 0);  // closed
}
