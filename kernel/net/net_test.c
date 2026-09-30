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
#include "errno.h"
#include "ktest.h"
#include "kfmt.h"   // klog_printf, for the line the fallback prints
#include "clocksource.h" // a reassembly timeout, aged by hand
#include "scheduler.h"   // the RTO test holds the preemption guard

// Frames live here rather than on the stack: the ring-0 frame budget is
// 1 KiB and these are close enough to it to be worth not testing.
static uint8_t g_frame[NET_FRAME_MAX];

// WHAT WAS SENT, not merely that something was. The transmit op is a
// function pointer, so a test can borrow it, run one frame in, and read
// the bytes the stack built -- which is the difference between a real
// assertion and one an unrelated ARP request can satisfy. It could, and
// did: "an echo request is answered" passed for a while on the ARP
// request the reply was waiting for, and never checked the reply at all.
static uint8_t g_sent[NET_FRAME_MAX];
static uint32_t g_sent_len;
static int g_sent_count;
static int (*g_real_transmit)(struct net_device *, const void *, uint32_t);

// Every frame of a burst, for a datagram that leaves in fragments.
#define TXLOG_MAX 16
static uint8_t g_txlog[TXLOG_MAX][NET_FRAME_MAX];
static uint32_t g_txlog_len[TXLOG_MAX];

static int capture_transmit(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev;
    if (len > sizeof g_sent) len = sizeof g_sent;
    if (g_sent_count < TXLOG_MAX) {
        k_memcpy(g_txlog[g_sent_count], frame, len);
        g_txlog_len[g_sent_count] = len;
    }
    k_memcpy(g_sent, frame, len);
    g_sent_len = len;
    g_sent_count++;
    return 0;   // never touches the hardware
}

static void capture_begin(struct net_device *dev) {
    g_real_transmit = dev->transmit;
    dev->transmit = capture_transmit;
    g_sent_len = 0;
    g_sent_count = 0;
}

// ALWAYS restore, on every exit from a test -- a KTEST_ASSERT returns
// early, so a test that captures must not assert before restoring or
// the device is left pointing at this file for the rest of the boot.
static void capture_end(struct net_device *dev) {
    if (g_real_transmit) dev->transmit = g_real_transmit;
    g_real_transmit = 0;
}

// The captured frame's ethertype, and a pointer past the IP header.
static uint16_t sent_ethertype(void) {
    return g_sent_len >= ETH_HDR_LEN ? net_ntohs(*(const uint16_t *)(g_sent + 12)) : 0;
}
static uint8_t sent_ip_proto(void) {
    return g_sent_len >= ETH_HDR_LEN + 20 ? g_sent[ETH_HDR_LEN + 9] : 0;
}
static const uint8_t *sent_transport(void) {
    if (g_sent_len < ETH_HDR_LEN + 20) return 0;
    uint32_t ihl = (uint32_t)(g_sent[ETH_HDR_LEN] & 0x0F) * 4;
    return g_sent_len >= ETH_HDR_LEN + ihl ? g_sent + ETH_HDR_LEN + ihl : 0;
}

// A device with an address, ADDRESSING ONE IF NOBODY HAS.
//
// Fourteen tests below need a device holding an address, and the kernel
// stopped guaranteeing one when `/bin/dhcp` took the job: a lease now
// arrives a moment after userland starts, so inheriting it would make
// this file's coverage depend on which finished first, and a skip says
// so only in a count nobody reads. The address stays -- there is no
// teardown here, and it is the one QEMU's user networking hands out.
static struct net_device *addressed_device(void) {
    for (int i = 0; i < net_device_count(); i++) {
        struct net_device *d = net_device_at(i);
        if (d && d->ip) return d;
    }
    struct net_device *d = net_device_count() ? net_device_at(0) : 0;
    if (!d) return 0;
    d->ip      = NET_IPV4(10, 0, 2, 15);
    d->netmask = NET_IPV4(255, 255, 255, 0);
    d->gateway = NET_IPV4(10, 0, 2, 2);
    klog_printf("net: ktest addressed %s -- no lease had arrived\n", d->name);
    return d;
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

// The sender every fixture below uses: ON THIS DEVICE'S OWN SUBNET.
// An off-subnet sender routes replies via the gateway, so what the test
// would observe is an ARP request for the gateway rather than the reply
// it meant to check -- which is exactly how the echo test came to pass
// without ever exercising an echo reply.
static uint32_t peer_ip(struct net_device *dev) {
    return (dev->ip & dev->netmask) | 99;
}

// An ARP request from that peer asking who has `target`.
static uint32_t arp_request(struct net_device *dev, uint32_t target, const uint8_t *dst) {
    uint32_t n = eth_frame(dev, dst, ETH_TYPE_ARP);
    uint8_t *p = g_frame + n;
    p[0] = 0; p[1] = 1;                     // Ethernet
    p[2] = 0x08; p[3] = 0x00;               // IPv4
    p[4] = NET_MAC_LEN; p[5] = 4;
    p[6] = 0; p[7] = 1;                     // request
    k_memcpy(p + 8, g_frame + 6, NET_MAC_LEN);
    uint32_t sender_ip = peer_ip(dev);
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
    uint32_t src = peer_ip(dev);
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


// An IPv4 + UDP datagram to `dst_ip`:`dport`. `corrupt` breaks the UDP
// checksum and `zero_sum` writes the "not computed" value, so one
// builder serves the accept case and both of its negatives.
static uint32_t udp_frame(struct net_device *dev, uint32_t dst_ip, uint16_t dport,
                          int corrupt, int zero_sum, const uint8_t *dst_mac) {
    uint32_t n = eth_frame(dev, dst_mac, ETH_TYPE_IPV4);
    uint8_t *ip = g_frame + n;
    const uint32_t payload = 8;
    const uint32_t udp_len = 8 + payload;
    uint32_t src = peer_ip(dev);

    k_memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (uint8_t)((20 + udp_len) >> 8); ip[3] = (uint8_t)(20 + udp_len);
    ip[8] = 64;
    ip[9] = IP_PROTO_UDP;
    ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);  ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dst_ip >> 24); ip[17] = (uint8_t)(dst_ip >> 16);
    ip[18] = (uint8_t)(dst_ip >> 8);  ip[19] = (uint8_t)dst_ip;
    uint16_t sum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;

    uint8_t *udp = ip + 20;
    udp[0] = 0xC0; udp[1] = 0x00;                                 // source port 49152
    udp[2] = (uint8_t)(dport >> 8); udp[3] = (uint8_t)dport;
    udp[4] = (uint8_t)(udp_len >> 8); udp[5] = (uint8_t)udp_len;
    udp[6] = udp[7] = 0;
    for (uint32_t i = 0; i < payload; i++) udp[8 + i] = (uint8_t)(0xA0 + i);

    if (!zero_sum) {
        // The pseudo-header the checksum covers and the wire does not.
        uint8_t pseudo[12];
        pseudo[0] = (uint8_t)(src >> 24); pseudo[1] = (uint8_t)(src >> 16);
        pseudo[2] = (uint8_t)(src >> 8);  pseudo[3] = (uint8_t)src;
        pseudo[4] = (uint8_t)(dst_ip >> 24); pseudo[5] = (uint8_t)(dst_ip >> 16);
        pseudo[6] = (uint8_t)(dst_ip >> 8);  pseudo[7] = (uint8_t)dst_ip;
        pseudo[8] = 0;
        pseudo[9] = IP_PROTO_UDP;
        pseudo[10] = (uint8_t)(udp_len >> 8);
        pseudo[11] = (uint8_t)udp_len;
        uint16_t c = net_checksum_two(pseudo, sizeof pseudo, udp, udp_len);
        if (!c) c = 0xFFFF;
        udp[6] = (uint8_t)(c >> 8); udp[7] = (uint8_t)c;
        if (corrupt) udp[7] ^= 0xFF;
    }
    return n + 20 + udp_len;
}


// An IPv4 + TCP segment from the peer. `flags` is the TCP flag byte;
// `seq`/`ack` are absolute; `payload_len` bytes of pattern follow.
// `at` is the payload's offset in the STREAM, and every byte names it:
// byte i is 'A' + ((at + i) % 26). A reassembly test whose segments all
// carry the same bytes cannot tell a correct order from a wrong one.
// `opt` (a multiple of 4 bytes) goes between the header and the payload,
// and `window` is the raw 16-bit field.
static uint32_t tcp_frame_full(struct net_device *dev, uint16_t sport, uint16_t dport,
                               uint8_t flags, uint32_t seq, uint32_t ack,
                               uint32_t payload_len, int corrupt, uint32_t at,
                               const uint8_t *opt, uint32_t optlen, uint16_t window) {
    uint32_t n = eth_frame(dev, 0, ETH_TYPE_IPV4);
    uint8_t *ip = g_frame + n;
    uint32_t hdr = 20 + optlen;
    uint32_t tcp_len = hdr + payload_len;
    uint32_t src = peer_ip(dev);

    k_memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (uint8_t)((20 + tcp_len) >> 8); ip[3] = (uint8_t)(20 + tcp_len);
    ip[8] = 64;
    ip[9] = IP_PROTO_TCP;
    ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);  ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dev->ip >> 24); ip[17] = (uint8_t)(dev->ip >> 16);
    ip[18] = (uint8_t)(dev->ip >> 8);  ip[19] = (uint8_t)dev->ip;
    uint16_t sum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;

    uint8_t *tcp = ip + 20;
    k_memset(tcp, 0, 20);
    tcp[0] = (uint8_t)(sport >> 8); tcp[1] = (uint8_t)sport;
    tcp[2] = (uint8_t)(dport >> 8); tcp[3] = (uint8_t)dport;
    tcp[4] = (uint8_t)(seq >> 24); tcp[5] = (uint8_t)(seq >> 16);
    tcp[6] = (uint8_t)(seq >> 8);  tcp[7] = (uint8_t)seq;
    tcp[8] = (uint8_t)(ack >> 24); tcp[9] = (uint8_t)(ack >> 16);
    tcp[10] = (uint8_t)(ack >> 8); tcp[11] = (uint8_t)ack;
    tcp[12] = (uint8_t)((hdr / 4) << 4);
    tcp[13] = flags;
    tcp[14] = (uint8_t)(window >> 8); tcp[15] = (uint8_t)window;
    if (optlen) k_memcpy(tcp + 20, opt, optlen);
    for (uint32_t i = 0; i < payload_len; i++) tcp[hdr + i] = (uint8_t)('A' + ((at + i) % 26));

    uint8_t pseudo[12];
    pseudo[0] = (uint8_t)(src >> 24); pseudo[1] = (uint8_t)(src >> 16);
    pseudo[2] = (uint8_t)(src >> 8);  pseudo[3] = (uint8_t)src;
    pseudo[4] = (uint8_t)(dev->ip >> 24); pseudo[5] = (uint8_t)(dev->ip >> 16);
    pseudo[6] = (uint8_t)(dev->ip >> 8);  pseudo[7] = (uint8_t)dev->ip;
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    pseudo[10] = (uint8_t)(tcp_len >> 8);
    pseudo[11] = (uint8_t)tcp_len;
    uint16_t c = net_checksum_two(pseudo, sizeof pseudo, tcp, tcp_len);
    if (!c) c = 0xFFFF;
    tcp[16] = (uint8_t)(c >> 8); tcp[17] = (uint8_t)c;
    if (corrupt) tcp[17] ^= 0xFF;

    return n + 20 + tcp_len;
}

static uint32_t tcp_frame_at(struct net_device *dev, uint16_t sport, uint16_t dport,
                             uint8_t flags, uint32_t seq, uint32_t ack,
                             uint32_t payload_len, int corrupt, uint32_t at) {
    return tcp_frame_full(dev, sport, dport, flags, seq, ack, payload_len, corrupt,
                          at, 0, 0, 8192);
}

static uint32_t tcp_frame(struct net_device *dev, uint16_t sport, uint16_t dport,
                          uint8_t flags, uint32_t seq, uint32_t ack,
                          uint32_t payload_len, int corrupt) {
    return tcp_frame_at(dev, sport, dport, flags, seq, ack, payload_len, corrupt, 0);
}

// The captured segment's flag byte and sequence, for asserting on what
// the state machine actually put on the wire.
static uint8_t sent_tcp_flags(void) {
    const uint8_t *t = sent_transport();
    return t ? t[13] : 0;
}
static uint32_t sent_tcp_seq(void) {
    const uint8_t *t = sent_transport();
    return t ? net_ntohl(*(const uint32_t *)(t + 4)) : 0;
}
static uint32_t sent_tcp_ack(void) {
    const uint8_t *t = sent_transport();
    return t ? net_ntohl(*(const uint32_t *)(t + 8)) : 0;
}
static uint16_t sent_tcp_sport(void) {
    const uint8_t *t = sent_transport();
    return t ? net_ntohs(*(const uint16_t *)t) : 0;
}
static uint16_t sent_tcp_window(void) {
    const uint8_t *t = sent_transport();
    return t ? net_ntohs(*(const uint16_t *)(t + 14)) : 0;
}
// The shift a captured SYN offered, or 0xFF when it carried no option 3.
static uint8_t sent_tcp_wscale(void) {
    const uint8_t *t = sent_transport();
    if (!t) return 0xFF;
    uint32_t doff = (uint32_t)(t[12] >> 4) * 4;
    for (uint32_t i = 20; i + 1 < doff; ) {
        if (t[i] == 0) break;
        if (t[i] == 1) { i++; continue; }
        if (t[i] == 3 && t[i + 1] == 3 && i + 2 < doff) return t[i + 2];
        if (t[i + 1] < 2) break;
        i += t[i + 1];
    }
    return 0xFF;
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

    capture_begin(dev);
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);
    int answered = g_sent_count;
    uint16_t type = sent_ethertype();
    // ARP opcode 2 is a REPLY. A stack that sent a second REQUEST here
    // would move the same counter.
    uint16_t op = g_sent_len >= ETH_HDR_LEN + 8
                  ? net_ntohs(*(const uint16_t *)(g_sent + ETH_HDR_LEN + 6)) : 0;

    // The same request for an address that is not ours. A stack that
    // answers this claims somebody else's address on the wire.
    g_sent_count = 0;
    len = arp_request(dev, peer_ip(dev) - 1, 0);
    eth_input(dev, g_frame, len);
    int wrongly_answered = g_sent_count;
    capture_end(dev);

    KTEST_ASSERT_EQ(answered, 1);
    KTEST_ASSERT_EQ(type, ETH_TYPE_ARP);
    KTEST_ASSERT_EQ(op, 2);
    KTEST_ASSERT_EQ(wrongly_answered, 0);
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

KTEST("net", "an echo request is answered with an echo REPLY") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    // The sender must be in the ARP cache, or the reply is deferred
    // behind a resolution nothing here will answer -- a precondition
    // this test establishes rather than inherits.
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    capture_begin(dev);
    len = icmp_echo(dev, dev->ip, 0, 0);
    eth_input(dev, g_frame, len);
    int sent = g_sent_count;
    uint16_t type = sent_ethertype();
    uint8_t proto = sent_ip_proto();
    const uint8_t *icmp = sent_transport();
    uint8_t icmp_type = icmp ? icmp[0] : 0xFF;
    uint16_t seq = icmp ? net_ntohs(*(const uint16_t *)(icmp + 6)) : 0;
    capture_end(dev);

    KTEST_ASSERT_EQ(sent, 1);
    KTEST_ASSERT_EQ(type, ETH_TYPE_IPV4);
    KTEST_ASSERT_EQ(proto, IP_PROTO_ICMP);
    KTEST_ASSERT_EQ(icmp_type, ICMP_ECHO_REPLY);
    // The sequence must come back UNCHANGED, or a sender cannot match
    // the reply to what it sent -- and a stack that rebuilt the message
    // instead of editing it would fail exactly here.
    KTEST_ASSERT_EQ(seq, 1);
}

KTEST("net", "a lone fragment is held, not answered; a bad checksum and somebody else's address are dropped") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);   // same precondition as above
    ipv4_frag_flush();

    capture_begin(dev);
    // More-fragments set: the start of a datagram, not a whole one that
    // happens to parse, so it is HELD rather than answered.
    len = icmp_echo(dev, dev->ip, 0x2000, 0);
    eth_input(dev, g_frame, len);
    int mf = g_sent_count;
    int mf_held = ipv4_frag_pending();
    ipv4_frag_flush();

    // A non-zero fragment offset, which is the other half of the same
    // rule and would pass a check that only looked at the MF bit.
    len = icmp_echo(dev, dev->ip, 0x0001, 0);
    eth_input(dev, g_frame, len);
    int offset = g_sent_count;
    int offset_held = ipv4_frag_pending();
    ipv4_frag_flush();

    // Somebody else's fragment costs nothing: the address is checked
    // before reassembly is.
    len = icmp_echo(dev, peer_ip(dev) - 1, 0x2000, 0);
    eth_input(dev, g_frame, len);
    int foreign_held = ipv4_frag_pending();

    len = icmp_echo(dev, dev->ip, 0, 1);   // corrupted header checksum
    eth_input(dev, g_frame, len);
    int bad_sum = g_sent_count;

    len = icmp_echo(dev, peer_ip(dev) - 1, 0, 0);  // not our address
    eth_input(dev, g_frame, len);
    int not_ours = g_sent_count;
    capture_end(dev);

    KTEST_ASSERT_EQ(mf, 0);
    KTEST_ASSERT_EQ(mf_held, 1);
    KTEST_ASSERT_EQ(offset, 0);
    KTEST_ASSERT_EQ(offset_held, 1);
    KTEST_ASSERT_EQ(foreign_held, 0);
    KTEST_ASSERT_EQ(bad_sum, 0);
    KTEST_ASSERT_EQ(not_ours, 0);
}

// --- fragmentation -----------------------------------------------------

// One UDP datagram, whole, which the fragment builder below slices.
// Byte i of the payload is i % 251: a prime, so a fragment placed one
// fragment-length off is wrong in its VALUES, not just its length.
static uint8_t g_dgram[4096];
static uint32_t g_dgram_len;

static void udp_datagram(struct net_device *dev, uint16_t dport, uint32_t payload) {
    uint32_t src = peer_ip(dev);
    uint32_t udp_len = 8 + payload;
    uint8_t *u = g_dgram;
    u[0] = 0xC0; u[1] = 0x00;
    u[2] = (uint8_t)(dport >> 8); u[3] = (uint8_t)dport;
    u[4] = (uint8_t)(udp_len >> 8); u[5] = (uint8_t)udp_len;
    u[6] = u[7] = 0;
    for (uint32_t i = 0; i < payload; i++) u[8 + i] = (uint8_t)(i % 251);
    uint8_t pseudo[12] = {
        (uint8_t)(src >> 24), (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src,
        (uint8_t)(dev->ip >> 24), (uint8_t)(dev->ip >> 16), (uint8_t)(dev->ip >> 8), (uint8_t)dev->ip,
        0, IP_PROTO_UDP, (uint8_t)(udp_len >> 8), (uint8_t)udp_len };
    uint16_t c = net_checksum_two(pseudo, sizeof pseudo, u, udp_len);
    if (!c) c = 0xFFFF;
    u[6] = (uint8_t)(c >> 8); u[7] = (uint8_t)c;
    g_dgram_len = udp_len;
}

// Bytes [off, off + n) of g_dgram as one fragment from the peer.
static uint32_t frag_frame(struct net_device *dev, uint16_t id, uint32_t off,
                           uint32_t n, int more) {
    uint32_t h = eth_frame(dev, 0, ETH_TYPE_IPV4);
    uint8_t *ip = g_frame + h;
    uint32_t src = peer_ip(dev);
    uint16_t ff = (uint16_t)((more ? IP_FLAG_MF : 0) | (off / 8));
    k_memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (uint8_t)((20 + n) >> 8); ip[3] = (uint8_t)(20 + n);
    ip[4] = (uint8_t)(id >> 8); ip[5] = (uint8_t)id;
    ip[6] = (uint8_t)(ff >> 8); ip[7] = (uint8_t)ff;
    ip[8] = 64;
    ip[9] = IP_PROTO_UDP;
    ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);  ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dev->ip >> 24); ip[17] = (uint8_t)(dev->ip >> 16);
    ip[18] = (uint8_t)(dev->ip >> 8);  ip[19] = (uint8_t)dev->ip;
    uint16_t sum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
    k_memcpy(ip + 20, g_dgram + off, n);
    return h + 20 + n;
}

static void send_frag(struct net_device *dev, uint16_t id, uint32_t off, uint32_t n, int more) {
    uint32_t len = frag_frame(dev, id, off, n, more);
    eth_input(dev, g_frame, len);
}

// 1 when `buf` holds exactly the payload udp_datagram() built.
static int payload_matches(const uint8_t *buf, uint32_t n) {
    if (n != g_dgram_len - 8) return 0;
    for (uint32_t i = 0; i < n; i++) if (buf[i] != (uint8_t)(i % 251)) return 0;
    return 1;
}

static uint8_t g_rxbuf[4096];

// Three fragments of a 4000-byte datagram: [0,1480) [1480,2960) [2960,4008).
#define FRAG_A 1480u

KTEST("net", "three fragments in order reassemble into one UDP datagram") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    ipv4_frag_flush();
    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7790, 0), 7790);

    udp_datagram(dev, 7790, 4000);
    send_frag(dev, 0x4101, 0, FRAG_A, 1);
    send_frag(dev, 0x4101, FRAG_A, FRAG_A, 1);
    int early = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    send_frag(dev, 0x4101, 2 * FRAG_A, g_dgram_len - 2 * FRAG_A, 0);

    int got = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    int ok = got > 0 && payload_matches(g_rxbuf, (uint32_t)got);
    int left = ipv4_frag_pending();
    net_sock_close(sock);

    KTEST_ASSERT_EQ(early, 0);              // nothing until the last one
    KTEST_ASSERT_EQ(got, 4000);
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(left, 0);               // the slot is given back
}

KTEST("net", "fragments in reverse order, with a duplicate, deliver once") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    ipv4_frag_flush();
    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7791, 0), 7791);

    udp_datagram(dev, 7791, 4000);
    send_frag(dev, 0x4102, 2 * FRAG_A, g_dgram_len - 2 * FRAG_A, 0);
    send_frag(dev, 0x4102, FRAG_A, FRAG_A, 1);
    send_frag(dev, 0x4102, FRAG_A, FRAG_A, 1);   // a duplicate is not an overlap
    send_frag(dev, 0x4102, 0, FRAG_A, 1);

    int got = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    int ok = got > 0 && payload_matches(g_rxbuf, (uint32_t)got);
    int again = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    net_sock_close(sock);

    KTEST_ASSERT_EQ(got, 4000);
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(again, 0);
}

KTEST("net", "a partial overlap drops the whole datagram") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    ipv4_frag_flush();
    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7792, 0), 7792);

    udp_datagram(dev, 7792, 4000);
    send_frag(dev, 0x4103, 0, FRAG_A, 1);
    send_frag(dev, 0x4103, FRAG_A - 8, FRAG_A, 1);   // one block into the first
    int after_overlap = ipv4_frag_pending();
    // The rest arriving must not complete it: fragment zero went with it.
    send_frag(dev, 0x4103, 2 * FRAG_A - 8, g_dgram_len - (2 * FRAG_A - 8), 0);
    send_frag(dev, 0x4103, FRAG_A, FRAG_A, 1);
    int got = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    ipv4_frag_flush();
    net_sock_close(sock);

    KTEST_ASSERT_EQ(after_overlap, 0);
    KTEST_ASSERT_EQ(got, 0);
}

KTEST("net", "a last fragment that ends before held data drops the datagram") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    ipv4_frag_flush();
    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7793, 0), 7793);

    // Held: [2960, 4008). Then a "last" fragment claiming the datagram
    // ends at 2960, and the first two fragments -- whose byte count
    // alone would add up to that claimed end and look complete.
    udp_datagram(dev, 7793, 4000);
    send_frag(dev, 0x4104, 2 * FRAG_A, g_dgram_len - 2 * FRAG_A, 1);
    send_frag(dev, 0x4104, FRAG_A, FRAG_A, 0);
    int after_lie = ipv4_frag_pending();
    send_frag(dev, 0x4104, 0, FRAG_A, 1);
    int got = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    ipv4_frag_flush();
    net_sock_close(sock);

    KTEST_ASSERT_EQ(after_lie, 0);
    KTEST_ASSERT_EQ(got, 0);
}

KTEST("net", "a stalled datagram times out with ICMP only if fragment zero arrived") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    uint32_t len = arp_request(dev, dev->ip, 0);   // the peer must resolve
    eth_input(dev, g_frame, len);
    ipv4_frag_flush();

    udp_datagram(dev, 7794, 4000);
    uint64_t now = clocksource_now_ns();
    capture_begin(dev);
    send_frag(dev, 0x4105, 0, FRAG_A, 1);
    ipv4_frag_expire(now + 1000000000ull);           // too soon
    int early = ipv4_frag_pending();
    ipv4_frag_expire(now + 31ull * 1000000000ull);
    int sent = g_sent_count;
    const uint8_t *icmp = sent_transport();
    uint8_t itype = icmp ? icmp[0] : 0xFF;
    uint8_t icode = icmp ? icmp[1] : 0xFF;
    uint16_t quoted_id = icmp ? net_ntohs(*(const uint16_t *)(icmp + 8 + 4)) : 0;
    int left = ipv4_frag_pending();

    // Without fragment zero there is no header to quote, so it goes
    // quietly (RFC 1122 3.3.2).
    g_sent_count = 0;
    send_frag(dev, 0x4106, FRAG_A, FRAG_A, 1);
    ipv4_frag_expire(now + 31ull * 1000000000ull);
    int sent_headless = g_sent_count;
    int left_headless = ipv4_frag_pending();
    capture_end(dev);

    KTEST_ASSERT_EQ(early, 1);
    KTEST_ASSERT_EQ(sent, 1);
    KTEST_ASSERT_EQ(itype, 11);             // time exceeded
    KTEST_ASSERT_EQ(icode, 1);              // fragment reassembly
    KTEST_ASSERT_EQ(quoted_id, 0x4105);
    KTEST_ASSERT_EQ(left, 0);
    KTEST_ASSERT_EQ(sent_headless, 0);
    KTEST_ASSERT_EQ(left_headless, 0);
}

KTEST("net", "a fifth datagram evicts the oldest, which then cannot complete") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    ipv4_frag_flush();
    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7795, 0), 7795);

    udp_datagram(dev, 7795, 4000);
    for (uint16_t i = 0; i < 5; i++) send_frag(dev, (uint16_t)(0x4200 + i), 0, FRAG_A, 1);
    int pending = ipv4_frag_pending();
    // The first is gone, so its remaining fragments start a new slot
    // missing fragment zero...
    send_frag(dev, 0x4200, FRAG_A, FRAG_A, 1);
    send_frag(dev, 0x4200, 2 * FRAG_A, g_dgram_len - 2 * FRAG_A, 0);
    int evicted = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    // ...while the newest is still whole-able.
    send_frag(dev, 0x4204, FRAG_A, FRAG_A, 1);
    send_frag(dev, 0x4204, 2 * FRAG_A, g_dgram_len - 2 * FRAG_A, 0);
    int newest = net_sock_recvfrom(sock, g_rxbuf, sizeof g_rxbuf, 0, 0);
    ipv4_frag_flush();
    net_sock_close(sock);

    KTEST_ASSERT_EQ(pending, 4);
    KTEST_ASSERT_EQ(evicted, 0);
    KTEST_ASSERT_EQ(newest, 4000);
}

KTEST("net", "a datagram over the MTU leaves in fragments that rebuild it") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    if (dev->mtu != NET_MTU) KTEST_SKIP("the fixture assumes a 1500-byte MTU");
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    static uint8_t payload[4000];
    for (uint32_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i % 251);

    capture_begin(dev);
    int rc = udp_output(dev, peer_ip(dev), 9, 7796, payload, sizeof payload);
    int sent = g_sent_count;
    capture_end(dev);
    KTEST_ASSERT_EQ(rc, 0);
    KTEST_ASSERT_EQ(sent, 3);               // 4008 bytes of UDP in 1480s

    // Put the pieces back by their OFFSETS and compare with what was
    // sent, header by header: same id, MF on all but the last, a valid
    // checksum, nothing over the MTU.
    static uint8_t rebuilt[4008];
    uint32_t covered = 0;
    uint16_t id0 = 0;
    for (int f = 0; f < 3; f++) {
        const uint8_t *ip = g_txlog[f] + ETH_HDR_LEN;
        uint32_t total = (uint32_t)(ip[2] << 8 | ip[3]);
        uint16_t ff = (uint16_t)(ip[6] << 8 | ip[7]);
        uint16_t id = (uint16_t)(ip[4] << 8 | ip[5]);
        uint32_t off = (uint32_t)(ff & IP_FRAG_MASK) * 8;
        if (f == 0) id0 = id;
        KTEST_ASSERT(total <= NET_MTU);
        KTEST_ASSERT_EQ(net_checksum(ip, 20), 0);
        KTEST_ASSERT_EQ(id, id0);
        KTEST_ASSERT_EQ((ff & IP_FLAG_MF) != 0, f < 2);
        KTEST_ASSERT(off + (total - 20) <= sizeof rebuilt);
        k_memcpy(rebuilt + off, ip + 20, total - 20);
        covered += total - 20;
    }
    KTEST_ASSERT_EQ(covered, 4008);
    KTEST_ASSERT_EQ(rebuilt[2] << 8 | rebuilt[3], 9);        // the UDP header, first
    KTEST_ASSERT_EQ(k_memcmp(rebuilt + 8, payload, sizeof payload), 0);
}

KTEST("net", "a socket's queue takes one maximal datagram, and bytes bound the rest") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7797, 0), 7797);

    static uint8_t big[SYS_NET_MSG_MAX];
    for (uint32_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i % 251);
    // Delivery reports the PORT matched, not the queue's verdict -- a
    // full queue is no reason for a port-unreachable -- so what was
    // kept is read back rather than taken from the return.
    static uint8_t out[SYS_NET_MSG_MAX];
    net_sock_deliver_udp(dev, peer_ip(dev), 1, 7797, big, sizeof big);
    net_sock_deliver_udp(dev, peer_ip(dev), 1, 7797, big, 1024); // over budget
    int first = net_sock_recvfrom(sock, out, sizeof out, 0, 0);
    int intact = first == (int)sizeof big && k_memcmp(out, big, sizeof big) == 0;
    int second = net_sock_recvfrom(sock, out, sizeof out, 0, 0);
    net_sock_deliver_udp(dev, peer_ip(dev), 1, 7797, big, 1024); // room again
    int third = net_sock_recvfrom(sock, out, sizeof out, 0, 0);
    net_sock_close(sock);

    KTEST_ASSERT_EQ(first, (int)sizeof big);   // an empty queue takes anything
    KTEST_ASSERT(intact);
    KTEST_ASSERT_EQ(second, 0);                 // the budget dropped it
    KTEST_ASSERT_EQ(third, 1024);               // reading gave the bytes back
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
    KTEST_ASSERT_EQ(net_sock_recvfrom(s, g_frame, sizeof g_frame, 0, 0), 0);
    net_sock_close(s);
    KTEST_ASSERT(net_sock_recvfrom(s, g_frame, sizeof g_frame, 0, 0) < 0);  // closed
}


KTEST("net", "a UDP datagram reaches the socket bound to its port") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7777, 0), 7777);

    capture_begin(dev);
    uint32_t len = udp_frame(dev, dev->ip, 7777, 0, 0, 0);
    eth_input(dev, g_frame, len);
    int sent = g_sent_count;
    capture_end(dev);

    uint8_t buf[32];
    uint32_t src = 0;
    uint16_t port = 0;
    int got = net_sock_recvfrom(sock, buf, sizeof buf, &src, &port);
    KTEST_ASSERT_EQ(got, 8);
    KTEST_ASSERT_EQ(src, peer_ip(dev));
    KTEST_ASSERT_EQ(port, 49152);          // the source port the frame carried
    KTEST_ASSERT_EQ(buf[0], 0xA0);
    // A delivered datagram is answered by the SOCKET, so nothing is
    // transmitted -- an unreachable report here would mean the demux
    // ran and the delivery did not.
    KTEST_ASSERT_EQ(sent, 0);

    net_sock_close(sock);
}

KTEST("net", "a datagram for an unbound port is answered with ICMP, and a corrupt one is not") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    // The sender must be resolvable or the report is deferred behind an
    // ARP nothing here will answer -- a precondition, not an assumption.
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    capture_begin(dev);
    len = udp_frame(dev, dev->ip, 7778, 0, 0, 0);   // nothing bound to 7778
    eth_input(dev, g_frame, len);
    int sent = g_sent_count;
    uint8_t proto = sent_ip_proto();
    const uint8_t *icmp = sent_transport();
    uint8_t itype = icmp ? icmp[0] : 0xFF;
    uint8_t icode = icmp ? icmp[1] : 0xFF;
    // The report must QUOTE the datagram it is about: the offending IP
    // header, then its first 8 bytes -- which for UDP is the whole
    // header, and is what lets the sender match the report to a socket.
    uint16_t quoted_dport = icmp ? net_ntohs(*(const uint16_t *)(icmp + 8 + 20 + 2)) : 0;

    // A CORRUPT datagram names a port that cannot be trusted, so it is
    // dropped rather than reported -- otherwise a flipped bit turns
    // into a report sent about a port nobody asked for.
    g_sent_count = 0;
    len = udp_frame(dev, dev->ip, 7778, 1, 0, 0);
    eth_input(dev, g_frame, len);
    int after_corrupt = g_sent_count;

    // A BROADCAST is never answered either: every host on the segment
    // would reply to a datagram addressed to all of them.
    g_sent_count = 0;
    len = udp_frame(dev, IP_BROADCAST, 7778, 0, 0, ETH_BROADCAST);
    eth_input(dev, g_frame, len);
    int after_broadcast = g_sent_count;
    capture_end(dev);

    KTEST_ASSERT_EQ(sent, 1);
    KTEST_ASSERT_EQ(proto, IP_PROTO_ICMP);
    KTEST_ASSERT_EQ(itype, 3);              // destination unreachable
    KTEST_ASSERT_EQ(icode, 3);              // port
    KTEST_ASSERT_EQ(quoted_dport, 7778);
    KTEST_ASSERT_EQ(after_corrupt, 0);
    KTEST_ASSERT_EQ(after_broadcast, 0);
}

KTEST("net", "a zero UDP checksum means not computed, and is accepted") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    int sock = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(sock >= 0);
    KTEST_ASSERT_EQ(net_sock_bind(sock, 0, 7779, 0), 7779);

    uint32_t len = udp_frame(dev, dev->ip, 7779, 0, 1, 0);
    eth_input(dev, g_frame, len);
    KTEST_ASSERT_EQ(net_sock_recvfrom(sock, g_frame, sizeof g_frame, 0, 0), 8);

    net_sock_close(sock);
}

KTEST("net", "binding: a named port, a taken one, and the ephemeral range") {
    int a = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    int b = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(a >= 0 && b >= 0);

    KTEST_ASSERT_EQ(net_sock_bind(a, 0, 7780, 0), 7780);
    KTEST_ASSERT(net_sock_bind(b, 0, 7780, 0) < 0);          // taken

    int eph = net_sock_bind(b, 0, 0, 0);
    KTEST_ASSERT(eph >= NET_PORT_EPHEMERAL_LO && eph <= NET_PORT_EPHEMERAL_HI);
    KTEST_ASSERT(eph != 7780);

    KTEST_ASSERT(net_sock_bind(b, 0, 0, "net99") < 0);       // no such device

    // An ICMP socket has no port to bind: its key is the identifier the
    // kernel assigned. Refusing beats accepting and ignoring, which
    // would leave a caller believing it had reserved something.
    int icmp = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_ICMP);
    KTEST_ASSERT(net_sock_bind(icmp, 0, 7781, 0) < 0);

    net_sock_close(a);
    net_sock_close(b);
    net_sock_close(icmp);
}

KTEST("net", "a closed socket releases its port") {
    // The leak this guards was real and presented far from its cause:
    // sockets were never returned to the table on close, so the THIRD
    // run of a program opening four of them could not open any, and the
    // symptom was socket() failing rather than anything about closing.
    int a = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT_EQ(net_sock_bind(a, 0, 7782, 0), 7782);
    net_sock_close(a);

    int b = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT_EQ(net_sock_bind(b, 0, 7782, 0), 7782);
    net_sock_close(b);
}


// A connection driven to ESTABLISHED, with the peer's view of it. The
// handshake is the precondition for every test below, so it is one
// helper rather than four copies -- and it ASSERTS its own steps, so a
// failure lands on the handshake rather than on whatever came after.
struct fake_peer { int sock; uint16_t our_port; uint32_t our_seq, peer_seq; };

// `peer_shift` < 0 sends a SYN+ACK with no window-scale option; otherwise
// it carries that shift. `our_shift`, when given, receives the shift our
// SYN offered, or 0xFF when it offered none.
static int establish_ws(struct net_device *dev, struct fake_peer *p, struct ktest_ctx *ctx,
                        int peer_shift, uint8_t *our_shift) {
    // The peer must be resolvable, or the SYN never leaves.
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    p->sock = net_sock_open(NET_AF_INET, NET_SOCK_STREAM, IP_PROTO_TCP);
    if (p->sock < 0) { ktest_fail(ctx, "net_sock_open", __FILE__, __LINE__); return 0; }

    capture_begin(dev);
    int rc = net_sock_connect(p->sock, peer_ip(dev), 8080);
    uint8_t flags = sent_tcp_flags();
    p->our_seq = sent_tcp_seq() + 1;      // the SYN takes one
    p->our_port = sent_tcp_sport();
    if (our_shift) *our_shift = sent_tcp_wscale();
    capture_end(dev);

    if (rc < 0) { ktest_fail_eq(ctx, "connect rc", rc, 0, __FILE__, __LINE__); return 0; }
    if (flags != 0x02) {        // SYN alone
        ktest_fail_eq(ctx, "syn flags", flags, 0x02, __FILE__, __LINE__);
        return 0;
    }

    p->peer_seq = 0x50000000u;
    capture_begin(dev);
    const uint8_t ws[4] = { 1, 3, 3, (uint8_t)(peer_shift < 0 ? 0 : peer_shift) };
    len = tcp_frame_full(dev, 8080, p->our_port, 0x12 /* SYN|ACK */,
                         p->peer_seq, p->our_seq, 0, 0, 0,
                         ws, peer_shift < 0 ? 0 : 4, 8192);
    eth_input(dev, g_frame, len);
    uint8_t ackf = sent_tcp_flags();
    uint32_t acked = sent_tcp_ack();
    capture_end(dev);
    p->peer_seq++;

    if (ackf != 0x10 || acked != p->peer_seq) {
        ktest_fail(ctx, "the SYN+ACK is acknowledged", __FILE__, __LINE__);
        return 0;
    }
    return 1;
}

static int establish(struct net_device *dev, struct fake_peer *p, struct ktest_ctx *ctx) {
    return establish_ws(dev, p, ctx, -1, 0);
}

// Close the socket AND let the peer acknowledge the FIN, so the
// connection block is reclaimed rather than left lingering. Seven tests
// against a four-block pool exhaust it otherwise -- which is how the
// orphan reclaim came to be written.
static void finish_close(struct net_device *dev, struct fake_peer *p) {
    net_sock_close(p->sock);
    // ACK **AND FIN**: acknowledging our FIN alone leaves the
    // connection in FIN_WAIT_2 waiting for the peer's, which is
    // correct and is not closed. The peer has to say it is done too.
    uint32_t len = tcp_frame(dev, 8080, p->our_port, 0x11 /* ACK|FIN */,
                             p->peer_seq, p->our_seq + 1, 0, 0);
    eth_input(dev, g_frame, len);
    // AND A VALID RESET, so a test that failed half way -- data never
    // acknowledged, so no FIN was ever sent -- does not leave its block
    // lingering and exhaust the pool for every test after it. A
    // connection already gone ignores it.
    len = tcp_frame(dev, 8080, p->our_port, 0x04 /* RST */, p->peer_seq + 1, 0, 0, 0);
    eth_input(dev, g_frame, len);
    tcp_tick();
}

KTEST("tcp", "an active open completes and reaches ESTABLISHED") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;
    KTEST_ASSERT_EQ(net_sock_connect_state(p.sock), 0);
    finish_close(dev, &p);
}

KTEST("tcp", "data arrives in order, is acknowledged, and reads back") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    capture_begin(dev);
    uint32_t len = tcp_frame(dev, 8080, p.our_port, 0x18 /* ACK|PSH */,
                             p.peer_seq, p.our_seq, 10, 0);
    eth_input(dev, g_frame, len);
    uint32_t acked = sent_tcp_ack();
    capture_end(dev);

    // The acknowledgement must cover the data, or the peer sends it again.
    KTEST_ASSERT_EQ(acked, p.peer_seq + 10);

    uint8_t buf[32];
    int got = net_sock_stream_recv(p.sock, buf, sizeof buf);
    KTEST_ASSERT_EQ(got, 10);
    KTEST_ASSERT_EQ(buf[0], 'A');
    KTEST_ASSERT_EQ(buf[9], 'J');
    finish_close(dev, &p);
}

// One segment of the fake peer's stream, `len` bytes at stream offset
// `at`. The payload names its own offset, so a byte delivered in the
// wrong place is visible as the wrong letter rather than as a length.
static uint32_t seg_at(struct net_device *dev, struct fake_peer *p,
                       uint8_t flags, uint32_t at, uint32_t len) {
    uint32_t n = tcp_frame_at(dev, 8080, p->our_port, flags,
                              p->peer_seq + at, p->our_seq, len, 0, at);
    eth_input(dev, g_frame, n);
    return n;
}

// What the stack acknowledged in response to one segment, as an offset
// into the peer's stream -- which is rcv_nxt, and so says exactly how
// much of the stream is contiguous.
static uint32_t acked_at(struct net_device *dev, struct fake_peer *p,
                         uint8_t flags, uint32_t at, uint32_t len) {
    capture_begin(dev);
    seg_at(dev, p, flags, at, len);
    uint32_t acked = sent_tcp_ack();
    capture_end(dev);
    return acked - p->peer_seq;
}

KTEST("tcp", "a segment past rcv_nxt is held, and delivered when the hole fills") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    // The second segment first. It is HELD, not delivered: the
    // acknowledgement must still name rcv_nxt -- a duplicate ACK, which
    // is the only thing this stack can say to ask for the hole, having
    // no SACK to describe what it already has.
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 10, 10), 0);
    uint8_t buf[64];
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), -EAGAIN);

    // The hole fills, and BOTH segments become readable at once.
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 0, 10), 20);
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), 20);
    KTEST_ASSERT_EQ(buf[0], 'A');       // stream offset 0
    KTEST_ASSERT_EQ(buf[9], 'J');
    KTEST_ASSERT_EQ(buf[10], 'K');      // stream offset 10, the held segment
    KTEST_ASSERT_EQ(buf[19], 'T');
    // finish_close()'s FIN has to sit exactly at rcv_nxt, so the peer's
    // view of its own stream has to move with what it sent -- a FIN
    // behind rcv_nxt is rejected and the block lingers out the pool.
    p.peer_seq += 20;
    finish_close(dev, &p);
}

KTEST("tcp", "segments arriving in reverse order reassemble into one stream") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 20, 10), 0);
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 10, 10), 0);
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 0, 10), 30);

    uint8_t buf[64];
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), 30);
    for (int i = 0; i < 30; i++) KTEST_ASSERT_EQ(buf[i], 'A' + (i % 26));
    p.peer_seq += 30;
    finish_close(dev, &p);
}

KTEST("tcp", "a FIN arriving over a hole does not end the stream early") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    // The last segment of a download carries the FIN, so this is what a
    // single loss near the end of a fetch looks like. Reporting the end
    // of stream here would truncate the file and look like a short read.
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x19 /* ACK|PSH|FIN */, 10, 10), 0);
    uint8_t buf[64];
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), -EAGAIN);

    // The hole fills: the data first, and the FIN only after it.
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 0, 10), 21);   // 20 bytes + the FIN
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), 20);
    KTEST_ASSERT_EQ(buf[19], 'T');
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), 0);
    p.peer_seq += 21;                   // 20 bytes, and the FIN's own number
    finish_close(dev, &p);
}

KTEST("tcp", "a retransmission overlapping held bytes delivers them once") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 10, 10), 0);
    // A peer with no SACK to read resends from the hole, and its
    // segment boundaries need not line up with the held range -- so the
    // retransmission covers the hole and PART of what is already held.
    // The stream must be 20 bytes, not 25: only the part past rcv_nxt
    // is still owed, and an overlap counted twice is a corrupted file.
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 0, 15), 20);

    uint8_t buf[64];
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), 20);
    for (int i = 0; i < 20; i++) KTEST_ASSERT_EQ(buf[i], 'A' + (i % 26));
    p.peer_seq += 20;
    finish_close(dev, &p);
}

KTEST("tcp", "a partial read moves the held bytes with it") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    // Held bytes live PAST the in-order ones in the same buffer, so a
    // read that compacts only the in-order part leaves them offset by
    // what it took -- delivered later as the wrong bytes rather than as
    // a wrong length, which no length assertion would catch.
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 0, 10), 10);
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 20, 10), 10);   // a hole at 10

    uint8_t buf[64];
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, 4), 4);
    KTEST_ASSERT_EQ(buf[0], 'A');
    KTEST_ASSERT_EQ(buf[3], 'D');

    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 10, 10), 30);
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), 26);
    for (int i = 0; i < 26; i++) KTEST_ASSERT_EQ(buf[i], 'A' + ((4 + i) % 26));
    p.peer_seq += 30;
    finish_close(dev, &p);
}

KTEST("tcp", "more holes than the range list holds are dropped, not mis-delivered") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    // TCP_OFO_MAX disjoint ranges, 10 bytes each with a 10-byte hole in
    // front of every one, which fills the list.
    for (uint32_t k = 1; k <= TCP_OFO_MAX; k++)
        KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 20 * k, 10), 0);
    // One more has nowhere to go. Dropping it is legal -- the peer still
    // holds it -- and the check that it was really dropped is that it
    // never appears in the stream below.
    const uint32_t end = 20 * TCP_OFO_MAX + 10;
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, end + 10, 10), 0);

    // Fill every hole the list did record.
    KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 0, 20), 30);
    for (uint32_t k = 1; k < TCP_OFO_MAX; k++)
        KTEST_ASSERT_EQ(acked_at(dev, &p, 0x18, 20 * k + 10, 10), 20 * (k + 1) + 10);

    // `end`, not end + 20: the range past it was refused rather than
    // kept and silently spliced on past a hole.
    static uint8_t buf[20 * TCP_OFO_MAX + 64];
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), (int)end);
    for (uint32_t i = 0; i < end; i++) KTEST_ASSERT_EQ(buf[i], 'A' + (i % 26));
    p.peer_seq += end;
    finish_close(dev, &p);
}

// Payload bytes of TCP in one captured frame.
static uint32_t tcp_payload_of(const uint8_t *frame, uint32_t flen) {
    if (flen < ETH_HDR_LEN + 20) return 0;
    const uint8_t *ip = frame + ETH_HDR_LEN;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    uint32_t total = (uint32_t)(ip[2] << 8 | ip[3]);
    if (ip[9] != IP_PROTO_TCP || total < ihl + 20) return 0;
    return total - ihl - (uint32_t)(ip[ihl + 12] >> 4) * 4;
}

// The first data segment THIS connection sent during a capture, by its
// source port. Not the last frame: the device is live, and another
// process's traffic (DHCP, NTP, an ARP) can land in the capture after
// ours -- 1 ktest run in 10 read an empty frame that way.
static uint32_t captured_payload_from(uint16_t sport) {
    for (int f = 0; f < g_sent_count && f < TXLOG_MAX; f++) {
        const uint8_t *ip = g_txlog[f] + ETH_HDR_LEN;
        if (g_txlog_len[f] < ETH_HDR_LEN + 40 || ip[9] != IP_PROTO_TCP) continue;
        const uint8_t *t = ip + (uint32_t)(ip[0] & 0x0F) * 4;
        if (net_ntohs(*(const uint16_t *)t) != sport) continue;
        uint32_t n = tcp_payload_of(g_txlog[f], g_txlog_len[f]);
        if (n) return n;
    }
    return 0;
}

// The peer acknowledges everything as it arrives until `total` bytes
// of ours have been sent and acknowledged; `first` were already out.
static void ack_all_sent(struct net_device *dev, struct fake_peer *p,
                         uint32_t first, uint32_t total) {
    uint32_t done = first;
    while (done < total) {
        capture_begin(dev);
        uint32_t len = tcp_frame_full(dev, 8080, p->our_port, 0x10, p->peer_seq,
                                      p->our_seq + done, 0, 0, 0, 0, 0, 8192);
        eth_input(dev, g_frame, len);
        uint32_t n = 0;
        for (int f = 0; f < g_sent_count && f < TXLOG_MAX; f++)
            n += tcp_payload_of(g_txlog[f], g_txlog_len[f]);
        capture_end(dev);
        if (!n) break;
        done += n;
    }
    uint32_t len = tcp_frame_full(dev, 8080, p->our_port, 0x10, p->peer_seq,
                                  p->our_seq + total, 0, 0, 0, 0, 0, 8192);
    eth_input(dev, g_frame, len);
    p->our_seq += total;
}

KTEST("tcp", "window scaling is offered, and honoured both ways when the peer offers it too") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    uint8_t ours = 0xFF;
    if (!establish_ws(dev, &p, ctx, 2, &ours)) return;

    // Our window, scaled: an ACK for one byte of data says how much
    // room there is, in units of 1 << ours.
    // And it must MEAN it: 1460 more bytes queued shrinks it by 1460,
    // to within one unit -- a field that ignored the shift would sit at
    // 65535 through both and promise eight times the ring.
    uint8_t sh = ours == 0xFF ? 0 : ours;
    capture_begin(dev);
    seg_at(dev, &p, 0x18, 0, 1);
    uint32_t w1 = sent_tcp_window();
    seg_at(dev, &p, 0x18, 1, 1460);
    uint32_t w2 = sent_tcp_window();
    capture_end(dev);
    uint32_t room = w1 << sh;
    uint32_t shrank = (w1 - w2) << sh;
    static uint8_t in[1461];
    capture_begin(dev);                     // the read's window ACK stays off the wire
    net_sock_stream_recv(p.sock, in, sizeof in);
    capture_end(dev);
    p.peer_seq += 1461;

    // Theirs: a window of 100 at shift 2 is 400 bytes, so a 1000-byte
    // send leaves in a 400-byte segment -- 100 if the shift were ignored.
    uint32_t len = tcp_frame_full(dev, 8080, p.our_port, 0x10, p.peer_seq, p.our_seq,
                                  0, 0, 0, 0, 0, 100);
    eth_input(dev, g_frame, len);
    static uint8_t out[1000];
    capture_begin(dev);
    int sent = net_sock_stream_send(p.sock, out, sizeof out);
    uint32_t seg = captured_payload_from(p.our_port);
    capture_end(dev);

    ack_all_sent(dev, &p, seg, 1000);      // so the close is clean
    finish_close(dev, &p);
    KTEST_ASSERT(ours != 0xFF);            // the SYN offered a shift
    KTEST_ASSERT(room > 65535);            // and the window uses it
    KTEST_ASSERT(shrank + (1u << sh) > 1460 && shrank < 1460 + (1u << sh));
    KTEST_ASSERT_EQ(sent, 1000);
    KTEST_ASSERT_EQ(seg, 400);
}

KTEST("tcp", "without the peer's window scale, nothing is shifted either way") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    capture_begin(dev);
    seg_at(dev, &p, 0x18, 0, 1);
    uint16_t win = sent_tcp_window();
    uint8_t one;
    net_sock_stream_recv(p.sock, &one, 1);  // captured, for the same reason
    capture_end(dev);
    p.peer_seq += 1;

    uint32_t len = tcp_frame_full(dev, 8080, p.our_port, 0x10, p.peer_seq, p.our_seq,
                                  0, 0, 0, 0, 0, 100);
    eth_input(dev, g_frame, len);
    static uint8_t out[1000];
    capture_begin(dev);
    net_sock_stream_send(p.sock, out, sizeof out);
    uint32_t seg = captured_payload_from(p.our_port);
    capture_end(dev);

    ack_all_sent(dev, &p, seg, 1000);
    finish_close(dev, &p);
    KTEST_ASSERT_EQ(win, 65535);           // the most an unscaled field says
    KTEST_ASSERT_EQ(seg, 100);             // their 100 means 100
}

KTEST("tcp", "the receive ring wraps, with a held range straddling the wrap") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    uint8_t ours = 0xFF;
    if (!establish_ws(dev, &p, ctx, 0, &ours)) return;
    KTEST_ASSERT(ours != 0xFF);

    // The ring's size, from the window an idle connection advertises.
    capture_begin(dev);
    seg_at(dev, &p, 0x18, 0, 1);
    // One byte is queued and the window rounds DOWN, so the ring is the
    // next power of two above what it says.
    uint32_t said = ((uint32_t)sent_tcp_window() << ours) + 1;
    uint32_t cap = 1;
    while (cap < said) cap <<= 1;
    static uint8_t buf[65536];
    int bad = 0;
    net_sock_stream_recv(p.sock, buf, 1);   // still captured: its window ACK must not reach SLIRP

    // Stream up to 2000 bytes short of the ring's end, reading as it
    // goes, so the next byte lands 2000 bytes before the wrap. CAPTURED
    // throughout: this runs long enough for the real receive path to
    // run, and an ACK leaking to QEMU's SLIRP for this made-up peer is
    // answered with a valid RST.
    uint32_t at = 1;
    while (at < cap - 2000) {
        uint32_t n = cap - 2000 - at < 1460 ? cap - 2000 - at : 1460;
        seg_at(dev, &p, 0x18, at, n);
        at += n;
        if (at % 32768 < 1460 || at == cap - 2000) {
            int got;
            while ((got = net_sock_stream_recv(p.sock, buf, sizeof buf)) > 0) {}
        }
    }
    capture_end(dev);
    // The later half first -- it crosses the wrap and is held -- then the
    // earlier half, which fills the hole in front of it.
    uint32_t held = acked_at(dev, &p, 0x18, at + 1400, 1400);
    uint32_t filled = acked_at(dev, &p, 0x18, at, 1400);
    int got = net_sock_stream_recv(p.sock, buf, sizeof buf);
    for (int i = 0; i < got; i++)
        if (buf[i] != 'A' + ((at + (uint32_t)i) % 26)) { bad = i + 1; break; }

    p.peer_seq += at + 2800;
    finish_close(dev, &p);
    KTEST_ASSERT_EQ(held, at);             // nothing past the hole acked
    KTEST_ASSERT_EQ(filled, at + 2800);
    KTEST_ASSERT_EQ(got, 2800);
    KTEST_ASSERT_EQ(bad, 0);
}

// Sequence number and payload of captured frame `f`, for the sender tests.
static uint32_t txlog_seq(int f) {
    const uint8_t *ip = g_txlog[f] + ETH_HDR_LEN;
    const uint8_t *t = ip + (uint32_t)(ip[0] & 0x0F) * 4;
    return net_ntohl(*(const uint32_t *)(t + 4));
}

// An ACK from the peer for `upto` bytes of ours, advertising `window`.
static void peer_ack(struct net_device *dev, struct fake_peer *p, uint32_t upto,
                     uint16_t window) {
    uint32_t len = tcp_frame_full(dev, 8080, p->our_port, 0x10, p->peer_seq,
                                  p->our_seq + upto, 0, 0, 0, 0, 0, window);
    eth_input(dev, g_frame, len);
}

// 20000 bytes of a pattern that names its own offset, for the send tests.
static uint8_t g_upload[70000];
static void upload_pattern(void) {
    for (uint32_t i = 0; i < sizeof g_upload; i++) g_upload[i] = (uint8_t)('a' + (i % 23));
}

KTEST("tcp", "a send fills the initial window, not one segment per ACK") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;
    upload_pattern();

    // The fake SYN+ACK sent no MSS, so segments are RFC 1122's 536 and
    // the initial window is ten of them; the peer's 32 KiB is no limit.
    peer_ack(dev, &p, 0, 32768);
    capture_begin(dev);
    int took = net_sock_stream_send(p.sock, g_upload, 20000);
    int burst = g_sent_count;
    // One ACK for all ten grows the window by ONE segment (slow start
    // counts ACKs, capped at a segment each), so eleven go next.
    g_sent_count = 0;
    peer_ack(dev, &p, 5360, 32768);
    int next = 0;
    for (int f = 0; f < g_sent_count && f < TXLOG_MAX; f++) {
        uint32_t n = tcp_payload_of(g_txlog[f], g_txlog_len[f]);
        if (n) next++;
    }
    capture_end(dev);

    ack_all_sent(dev, &p, 5360 + 11 * 536, 20000);
    finish_close(dev, &p);
    KTEST_ASSERT_EQ(took, 20000);
    KTEST_ASSERT_EQ(burst, 10);
    KTEST_ASSERT_EQ(next, 11);
}

KTEST("tcp", "the third duplicate ACK resends the first unacked segment, and not before") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;
    const uint32_t base = p.our_seq;
    upload_pattern();

    peer_ack(dev, &p, 0, 32768);
    capture_begin(dev);
    net_sock_stream_send(p.sock, g_upload, 5360);    // exactly the initial window
    // Segment one is "lost": the peer acknowledges nothing, three times.
    g_sent_count = 0;
    peer_ack(dev, &p, 0, 32768);
    peer_ack(dev, &p, 0, 32768);
    int before_third = g_sent_count;
    peer_ack(dev, &p, 0, 32768);
    int after_third = g_sent_count;
    uint32_t seq = after_third ? txlog_seq(0) : 0;
    uint32_t len = after_third ? tcp_payload_of(g_txlog[0], g_txlog_len[0]) : 0;
    capture_end(dev);

    ack_all_sent(dev, &p, 5360, 5360);
    finish_close(dev, &p);
    KTEST_ASSERT_EQ(before_third, 0);
    KTEST_ASSERT_EQ(after_third, 1);
    KTEST_ASSERT_EQ(seq, base);          // snd_una, not anything later
    KTEST_ASSERT_EQ(len, 536);
}

KTEST("tcp", "a partial ACK in recovery resends the next hole at once") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;
    const uint32_t base = p.our_seq;
    upload_pattern();

    peer_ack(dev, &p, 0, 32768);
    capture_begin(dev);
    net_sock_stream_send(p.sock, g_upload, 5360);
    for (int i = 0; i < 3; i++) peer_ack(dev, &p, 0, 32768);   // into recovery
    // Segments one and three were lost: the resent first one fills the
    // first hole, and the ACK stops at the second.
    g_sent_count = 0;
    peer_ack(dev, &p, 2 * 536, 32768);
    uint32_t seq = g_sent_count ? txlog_seq(0) : 0;
    capture_end(dev);

    ack_all_sent(dev, &p, 5360, 5360);
    finish_close(dev, &p);
    KTEST_ASSERT(seq == base + 2 * 536);  // the hole, without a timeout
}

KTEST("tcp", "a retransmission timeout goes back to snd_una with one segment of window") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;
    const uint32_t base = p.our_seq;
    upload_pattern();

    peer_ack(dev, &p, 0, 32768);
    capture_begin(dev);
    net_sock_stream_send(p.sock, g_upload, 5360);
    // Nothing is acknowledged. Past the 200 ms floor the timer fires --
    // CAPTURED throughout, or the retransmission reaches SLIRP, and with
    // preemption off, or another process's socket call runs tcp_tick()
    // first and this one finds nothing due.
    scheduler_preempt_disable();
    uint64_t until = clocksource_now_ns() + 300000000ull;
    while (clocksource_now_ns() < until) {}
    g_sent_count = 0;
    tcp_tick();
    scheduler_preempt_enable();
    int resent = g_sent_count;
    uint32_t seq = resent ? txlog_seq(0) : 0;
    // One ACK for it doubles the window to two.
    g_sent_count = 0;
    peer_ack(dev, &p, 536, 32768);
    int next = g_sent_count;
    uint32_t next_seq = next ? txlog_seq(0) : 0;
    capture_end(dev);

    ack_all_sent(dev, &p, 3 * 536, 5360);
    finish_close(dev, &p);
    KTEST_ASSERT_EQ(resent, 1);
    KTEST_ASSERT_EQ(seq, base);
    KTEST_ASSERT_EQ(next, 2);
    KTEST_ASSERT_EQ(next_seq, base + 536);   // sent again: go-back-N
}

KTEST("tcp", "the send ring wraps, and every byte leaves where it belongs") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;
    upload_pattern();

    // A peer window of four segments keeps every burst inside the
    // capture log; 70000 bytes crosses a 64 KiB ring's end.
    peer_ack(dev, &p, 0, 4 * 536);
    uint32_t queued = 0, acked = 0;
    int bad = 0, rounds = 0;
    while (acked < sizeof g_upload && rounds++ < 400) {
        capture_begin(dev);
        if (queued < sizeof g_upload) {
            int n = net_sock_stream_send(p.sock, g_upload + queued,
                                         (uint32_t)sizeof g_upload - queued);
            if (n > 0) queued += (uint32_t)n;
        }
        peer_ack(dev, &p, acked, 4 * 536);
        uint32_t got = 0;
        for (int f = 0; f < g_sent_count && f < TXLOG_MAX; f++) {
            uint32_t n = tcp_payload_of(g_txlog[f], g_txlog_len[f]);
            const uint8_t *ip = g_txlog[f] + ETH_HDR_LEN;
            const uint8_t *t = ip + (uint32_t)(ip[0] & 0x0F) * 4;
            const uint8_t *pl = t + (uint32_t)(t[12] >> 4) * 4;
            uint32_t off = txlog_seq(f) - p.our_seq;
            for (uint32_t i = 0; i < n && !bad; i++)
                if (off + i >= sizeof g_upload || pl[i] != g_upload[off + i]) bad = (int)(off + i) + 1;
            if (off + n > acked + got) got = off + n - acked;
        }
        capture_end(dev);
        acked += got;
    }
    peer_ack(dev, &p, acked, 8192);
    p.our_seq += acked;

    finish_close(dev, &p);
    KTEST_ASSERT_EQ(bad, 0);
    KTEST_ASSERT_EQ(acked, (uint32_t)sizeof g_upload);
}

KTEST("tcp", "a corrupted segment is dropped entirely") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    capture_begin(dev);
    uint32_t len = tcp_frame(dev, 8080, p.our_port, 0x18, p.peer_seq, p.our_seq, 10, 1);
    eth_input(dev, g_frame, len);
    int sent = g_sent_count;
    capture_end(dev);

    // Not even an ACK: the segment's sequence numbers cannot be trusted.
    KTEST_ASSERT_EQ(sent, 0);
    uint8_t buf[32];
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), -EAGAIN);
    finish_close(dev, &p);
}

KTEST("tcp", "a FIN ends the stream, and a read reports it as 0 rather than an error") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    struct fake_peer p;
    if (!establish(dev, &p, ctx)) return;

    uint32_t len = tcp_frame(dev, 8080, p.our_port, 0x11 /* ACK|FIN */,
                             p.peer_seq, p.our_seq, 0, 0);
    eth_input(dev, g_frame, len);
    p.peer_seq++;              // a FIN takes a sequence number

    uint8_t buf[32];
    // END OF STREAM IS 0, which is what every reader of every other
    // stream in this system already tests for. An error here would make
    // a normal close look like a failure.
    KTEST_ASSERT_EQ(net_sock_stream_recv(p.sock, buf, sizeof buf), 0);
    // Deliberately no check of the connection's STATE here. End-of-
    // stream is the property, the state is only evidence for it, and
    // reading it needs a connection INDEX -- which an earlier version
    // of this test guessed as 0 and got wrong the moment another test
    // held that block.
    finish_close(dev, &p);
}

KTEST("tcp", "a RST answering the SYN is refused, not merely reset") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");

    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    int sock = net_sock_open(NET_AF_INET, NET_SOCK_STREAM, IP_PROTO_TCP);
    KTEST_ASSERT(sock >= 0);

    capture_begin(dev);
    net_sock_connect(sock, peer_ip(dev), 9999);
    uint16_t port = sent_tcp_sport();
    uint32_t seq = sent_tcp_seq() + 1;
    capture_end(dev);

    len = tcp_frame(dev, 9999, port, 0x14 /* ACK|RST */, 0, seq, 0, 0);
    eth_input(dev, g_frame, len);

    // The distinction the caller acts on: "nothing is listening there"
    // sends you to check the port, "reset" sends you to try again.
    KTEST_ASSERT_EQ(net_sock_connect_state(sock), -ECONNREFUSED);
    net_sock_close(sock);
}

KTEST("tcp", "a stream socket refuses the wrong protocol, and a datagram refuses connect") {
    KTEST_ASSERT(net_sock_open(NET_AF_INET, NET_SOCK_STREAM, IP_PROTO_UDP) < 0);
    int udp = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(udp >= 0);
    KTEST_ASSERT(net_sock_connect(udp, NET_IPV4(10, 0, 2, 2), 80) < 0);
    KTEST_ASSERT(!net_sock_is_stream(udp));
    net_sock_close(udp);
}


// A client's side of a passive open: SYN in, read the SYN+ACK, ACK it.
// Does NOT accept -- the caller decides when, which is what lets the
// backlog test fill the queue without needing a socket per connection.
// Returns 1, or 0 having already failed the test.
static int handshake_from_outside(struct net_device *dev, struct ktest_ctx *ctx,
                                  uint16_t client_port, uint32_t *out_client_seq) {
    uint32_t peer_seq = 0x60000000u + client_port;

    capture_begin(dev);
    uint32_t len = tcp_frame(dev, client_port, 8081, 0x02 /* SYN */, peer_seq, 0, 0, 0);
    eth_input(dev, g_frame, len);
    uint8_t flags = sent_tcp_flags();
    uint32_t their_seq = sent_tcp_seq();
    uint32_t their_ack = sent_tcp_ack();
    capture_end(dev);

    if (flags != 0x12) {          // SYN|ACK
        ktest_fail_eq(ctx, "synack flags", flags, 0x12, __FILE__, __LINE__);
        return 0;
    }
    if (their_ack != peer_seq + 1) {
        ktest_fail_eq(ctx, "synack ack", (int64_t)their_ack,
                      (int64_t)(peer_seq + 1), __FILE__, __LINE__);
        return 0;
    }

    len = tcp_frame(dev, client_port, 8081, 0x10 /* ACK */,
                    peer_seq + 1, their_seq + 1, 0, 0);
    eth_input(dev, g_frame, len);
    if (out_client_seq) *out_client_seq = peer_seq + 1;
    return 1;
}

// Tear a connection down deterministically: a VALID reset from the
// peer (the sequence must be where the next byte was expected, or it is
// ignored -- see tcp.c) drives it to CLOSED, and tcp_tick() then
// reclaims the block. Without this a test leaves an orphan lingering
// for two seconds, and eleven tests in a few milliseconds exhaust the
// pool -- which is how this was found, with a later test reporting
// -ENOSPC from listen().
static void abort_from_peer(struct net_device *dev, uint16_t client_port,
                            uint16_t local_port, uint32_t seq) {
    uint32_t len = tcp_frame(dev, client_port, local_port, 0x04 /* RST */,
                             seq, 0, 0, 0);
    eth_input(dev, g_frame, len);
    tcp_tick();
}

KTEST("tcp", "listen refuses an unbound socket, and accepts a bound one") {
    int s = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    KTEST_ASSERT(net_sock_listen(s) < 0);          // not a stream
    net_sock_close(s);

    s = net_sock_open(NET_AF_INET, NET_SOCK_STREAM, IP_PROTO_TCP);
    KTEST_ASSERT(s >= 0);
    // A port the kernel picked is one no client could know to connect
    // to, so an unbound listener is refused rather than given one.
    KTEST_ASSERT(net_sock_listen(s) < 0);
    KTEST_ASSERT_EQ(net_sock_bind(s, 0, 8081, 0), 8081);
    KTEST_ASSERT_EQ(net_sock_listen(s), 0);
    KTEST_ASSERT_EQ(net_sock_accept(s), -EAGAIN);  // nobody has connected
    net_sock_close(s);
}

KTEST("tcp", "a half-open connection is not offered to accept") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    int lis = net_sock_open(NET_AF_INET, NET_SOCK_STREAM, IP_PROTO_TCP);
    KTEST_ASSERT_EQ(net_sock_bind(lis, 0, 8081, 0), 8081);
    KTEST_ASSERT_EQ(net_sock_listen(lis), 0);

    // A SYN, answered with SYN+ACK, and NOTHING MORE. The handshake is
    // two thirds done; handing this to accept() would hand over a
    // connection the client has not confirmed.
    capture_begin(dev);
    len = tcp_frame(dev, 40009, 8081, 0x02, 0x61000000u, 0, 0, 0);
    eth_input(dev, g_frame, len);
    uint8_t flags = sent_tcp_flags();
    capture_end(dev);

    KTEST_ASSERT_EQ(flags, 0x12);
    KTEST_ASSERT_EQ(net_sock_accept(lis), -EAGAIN);

    abort_from_peer(dev, 40009, 8081, 0x61000001u);
    net_sock_close(lis);
    tcp_tick();
}

KTEST("tcp", "a passive open completes, and the listener keeps listening") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    int lis = net_sock_open(NET_AF_INET, NET_SOCK_STREAM, IP_PROTO_TCP);
    KTEST_ASSERT_EQ(net_sock_bind(lis, 0, 8081, 0), 8081);
    KTEST_ASSERT_EQ(net_sock_listen(lis), 0);

    uint32_t client_seq = 0;
    if (!handshake_from_outside(dev, ctx, 40001, &client_seq)) { net_sock_close(lis); return; }
    int conn = net_sock_accept(lis);
    KTEST_ASSERT(conn >= 0);
    KTEST_ASSERT(conn != lis);      // accept() returns a SEPARATE socket

    uint32_t peer_out = 0;
    uint16_t port_out = 0;
    net_sock_peer(conn, &peer_out, &port_out);
    KTEST_ASSERT_EQ(peer_out, peer_ip(dev));
    KTEST_ASSERT_EQ(port_out, 40001);

    // Data goes to the ACCEPTED socket, never to the listener.
    len = tcp_frame(dev, 40001, 8081, 0x18, client_seq, 0, 6, 0);
    eth_input(dev, g_frame, len);
    uint8_t buf[16];
    KTEST_ASSERT_EQ(net_sock_stream_recv(conn, buf, sizeof buf), 6);
    KTEST_ASSERT_EQ(buf[0], 'A');

    // AND THE LISTENER IS STILL LISTENING. Asserted behaviourally --
    // a second client connects and is accepted -- rather than by
    // reading a state variable, because "it still works" is the
    // property and the variable is only evidence for it.
    uint32_t seq2 = 0;
    if (!handshake_from_outside(dev, ctx, 40002, &seq2)) { net_sock_close(lis); return; }
    int conn2 = net_sock_accept(lis);
    KTEST_ASSERT(conn2 >= 0);
    KTEST_ASSERT(conn2 != conn);

    net_sock_close(conn);
    net_sock_close(conn2);
    abort_from_peer(dev, 40001, 8081, client_seq + 6);
    abort_from_peer(dev, 40002, 8081, seq2);
    net_sock_close(lis);
    tcp_tick();
}

KTEST("tcp", "a full backlog drops the SYN rather than refusing it") {
    struct net_device *dev = addressed_device();
    if (!dev) KTEST_SKIP("no network device with an address");
    uint32_t len = arp_request(dev, dev->ip, 0);
    eth_input(dev, g_frame, len);

    int lis = net_sock_open(NET_AF_INET, NET_SOCK_STREAM, IP_PROTO_TCP);
    KTEST_ASSERT_EQ(net_sock_bind(lis, 0, 8081, 0), 8081);
    KTEST_ASSERT_EQ(net_sock_listen(lis), 0);

    // Fill it: handshakes completed and left UNACCEPTED, which is what
    // a backlog holds.
    for (uint16_t i = 0; i < TCP_BACKLOG; i++) {
        uint32_t seq = 0;
        if (!handshake_from_outside(dev, ctx, (uint16_t)(41000 + i), &seq)) {
            net_sock_close(lis);
            return;
        }
    }

    // One more must be met with SILENCE. A RST here would turn a
    // momentary burst into a hard failure for the client, where a drop
    // lets its own SYN retransmission succeed a moment later.
    capture_begin(dev);
    len = tcp_frame(dev, 42000, 8081, 0x02, 0x70000000u, 0, 0, 0);
    eth_input(dev, g_frame, len);
    int answered = g_sent_count;
    capture_end(dev);
    KTEST_ASSERT_EQ(answered, 0);

    // Drain what the backlog held, so the pool is not left full.
    for (uint16_t i = 0; i < TCP_BACKLOG; i++) {
        int c2 = net_sock_accept(lis);
        if (c2 >= 0) net_sock_close(c2);
        abort_from_peer(dev, (uint16_t)(41000 + i), 8081,
                        0x60000001u + (uint32_t)(41000 + i));
    }
    net_sock_close(lis);
    tcp_tick();
}
