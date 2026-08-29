// ICMP: echo request answered, echo reply delivered to a socket.
//
// Only the two echo types. An unreachable/time-exceeded message is
// dropped rather than reported upward, because nothing above has a
// path to be told on yet -- when UDP or TCP lands, that report becomes
// a real delivery rather than another branch here.
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
