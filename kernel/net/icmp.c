// ICMP: echo request answered, echo reply delivered to a socket, and
// "nothing is listening there" sent when UDP has nobody to deliver to.
//
// An unreachable message ARRIVING is still dropped rather than reported
// upward: a socket has no error queue to put it on, so a client sees a
// timeout where it could have seen a refusal. That is the remaining
// half of this file's story and it is a roadmap item, not an oversight
// -- sending the report is what stops OTHER hosts timing out against
// us, and it is the half that is ours to get right.
//
// THE REPLY IS THE REQUEST, EDITED. The identifier, sequence and
// payload all have to come back unchanged for the sender to match the
// reply to what it sent, so building a fresh message would mean
// copying every field anyway -- and forgetting one is a ping that
// times out against a machine that answered.
#include "net.h"
#include "netdev.h"
#include "string.h"

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

_Static_assert(sizeof(struct icmp_header) == 8, "ICMP echo header is 8 bytes on the wire");

#define ICMP_MAX 1400

#define ICMP_DEST_UNREACHABLE 3
#define ICMP_CODE_PORT        3

// What a report carries back: the offending IPv4 header plus the first
// 8 bytes after it, which for UDP is the whole header -- so the sender
// can match the report to the socket that sent it. RFC 792's minimum,
// and every stack sends exactly this much.
#define UNREACH_QUOTE 8

static uint8_t g_msg[ICMP_MAX];

void icmp_input(struct net_device *dev, uint32_t src_ip, const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct icmp_header) || len > ICMP_MAX) return;
    if (net_checksum(pkt, len) != 0) return;

    struct icmp_header h;
    k_memcpy(&h, pkt, sizeof h);

    if (h.type == ICMP_ECHO_REQUEST) {
        k_memcpy(g_msg, pkt, len);
        struct icmp_header *r = (struct icmp_header *)g_msg;
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = net_htons(net_checksum(g_msg, len));
        ipv4_output(dev, src_ip, IP_PROTO_ICMP, g_msg, len);
        return;
    }

    if (h.type == ICMP_ECHO_REPLY)
        net_sock_deliver(IP_PROTO_ICMP, src_ip, pkt, len);
}

void icmp_send_port_unreachable(struct net_device *dev, uint32_t src_ip,
                                const uint8_t *ip_datagram, uint32_t ip_len) {
    // The report quotes the datagram AS IT ARRIVED -- header included,
    // options and all -- because what the sender matches against is the
    // header it sent, not one rebuilt from its fields.
    uint32_t ihl = (uint32_t)(ip_datagram[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > ip_len) return;

    uint32_t after = ip_len - ihl;
    uint32_t quote = after < UNREACH_QUOTE ? after : UNREACH_QUOTE;
    uint32_t body = ihl + quote;
    if (sizeof(struct icmp_header) + body > ICMP_MAX) return;

    struct icmp_header *h = (struct icmp_header *)g_msg;
    h->type = ICMP_DEST_UNREACHABLE;
    h->code = ICMP_CODE_PORT;
    h->checksum = 0;
    h->id = 0;      // "unused" in RFC 792, and it must be zero
    h->seq = 0;
    k_memcpy(g_msg + sizeof *h, ip_datagram, body);

    uint32_t total = (uint32_t)sizeof *h + body;
    h->checksum = net_htons(net_checksum(g_msg, total));
    ipv4_output(dev, src_ip, IP_PROTO_ICMP, g_msg, total);
}
