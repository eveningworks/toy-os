// The socket table behind SYS_SOCKET/SYS_SENDTO/SYS_RECVFROM.
//
// ONE PROTOCOL TODAY: AF_INET + SOCK_DGRAM + IPPROTO_ICMP, i.e. what
// `ping` needs. The shape is Linux's "ping socket" (its
// IPPROTO_ICMP datagram socket), NOT a raw socket, and the difference
// is the whole design here: THE KERNEL OWNS THE ICMP HEADER. An app
// sends a payload and receives a payload; type, code, checksum and
// identifier are the stack's.
//
// That matters more here than it does on Linux. A raw socket lets an
// app emit any ICMP type it likes -- redirects, forged unreachables --
// and Linux gates that behind CAP_NET_RAW. toy-os has no privilege
// model at all (see docs/roadmap.md's multi-user track), so the only
// available gate is not offering the primitive. A raw socket can
// arrive with the uid that would police it.
//
// THE IDENTIFIER IS THE DEMUX KEY. Each socket gets one, and a reply
// is delivered to the socket whose id it carries -- which is exactly
// what the ICMP echo identifier field is for, and why two `ping`s can
// run at once without reading each other's replies.
//
// A SOCKET IS AN INDEX, NOT A POINTER. The fd table stores the index,
// so a stale or forged fd cannot dereference anything -- the same
// reason pipe and pty fds store one.
#include "net.h"
#include "netdev.h"
#include "string.h"
#include "errno.h"

#define SOCK_MAX      8
#define SOCK_MSG_MAX  1472   // one datagram inside a 1500-byte MTU
#define SOCK_QUEUE    2      // in-flight replies per socket

// Identifiers start high enough to be recognisable in a packet capture
// and low enough to stay clear of anything a real host is using.
#define SOCK_ID_BASE  0x7001

struct sock_msg {
    uint32_t src;
    uint32_t len;
    uint8_t data[SOCK_MSG_MAX];
};

struct socket {
    uint8_t in_use;
    uint8_t proto;
    uint16_t id;
    uint16_t seq;
    struct sock_msg q[SOCK_QUEUE];
    int head, tail;   // tail == head means empty; one slot is left unused
};

static struct socket g_socks[SOCK_MAX];

struct icmp_echo {
    uint8_t type, code;
    uint16_t checksum;
    uint16_t id, seq;
} __attribute__((packed));

static uint8_t g_out[sizeof(struct icmp_echo) + SOCK_MSG_MAX];

int net_sock_open(int domain, int type, int protocol) {
    if (domain != NET_AF_INET || type != NET_SOCK_DGRAM) return -EINVAL;
    if (protocol != IP_PROTO_ICMP) return -EINVAL;

    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].in_use) continue;
        k_memset(&g_socks[i], 0, sizeof g_socks[i]);
        g_socks[i].in_use = 1;
        g_socks[i].proto = IP_PROTO_ICMP;
        g_socks[i].id = (uint16_t)(SOCK_ID_BASE + i);
        return i;
    }
    return -ENOSPC;
}

void net_sock_close(int sock) {
    if (sock < 0 || sock >= SOCK_MAX) return;
    k_memset(&g_socks[sock], 0, sizeof g_socks[sock]);
}

int net_sock_sendto(int sock, uint32_t dst_ip, const void *buf, uint32_t len) {
    if (sock < 0 || sock >= SOCK_MAX || !g_socks[sock].in_use) return -EBADF;
    if (len > SOCK_MSG_MAX) return -EINVAL;
    if (!dst_ip) return -EINVAL;

    // THE SEQUENCE ADVANCES ONLY WHEN A PACKET REALLY LEAVES. A send
    // that comes back -EAGAIN is an ARP resolution in progress, and the
    // caller retries the SAME request -- counting those would make the
    // numbers a record of attempts rather than of packets, so a capture
    // shows a ping starting at 4 and every seq skipping unpredictably.
    struct socket *s = &g_socks[sock];
    uint16_t seq = (uint16_t)(s->seq + 1);

    struct icmp_echo *h = (struct icmp_echo *)g_out;
    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->checksum = 0;
    h->id = net_htons(s->id);
    h->seq = net_htons(seq);
    if (len) k_memcpy(g_out + sizeof *h, buf, len);

    uint32_t total = sizeof *h + len;
    h->checksum = net_htons(net_checksum(g_out, total));

    int rc = ipv4_output(0, dst_ip, IP_PROTO_ICMP, g_out, total);
    if (rc < 0) return rc;
    s->seq = seq;
    return (int)len;
}

int net_sock_recvfrom(int sock, void *buf, uint32_t cap, uint32_t *out_src) {
    if (sock < 0 || sock >= SOCK_MAX || !g_socks[sock].in_use) return -EBADF;
    struct socket *s = &g_socks[sock];
    if (s->head == s->tail) return 0;   // nothing queued; never blocks

    struct sock_msg *m = &s->q[s->head];
    uint32_t n = m->len < cap ? m->len : cap;
    if (buf && n) k_memcpy(buf, m->data, n);
    if (out_src) *out_src = m->src;
    s->head = (s->head + 1) % SOCK_QUEUE;
    return (int)n;
}

int net_sock_deliver(uint8_t proto, uint32_t src_ip, const uint8_t *data, uint32_t len) {
    if (proto != IP_PROTO_ICMP || len < sizeof(struct icmp_echo)) return 0;

    struct icmp_echo h;
    k_memcpy(&h, data, sizeof h);
    uint16_t id = net_ntohs(h.id);

    for (int i = 0; i < SOCK_MAX; i++) {
        struct socket *s = &g_socks[i];
        if (!s->in_use || s->id != id) continue;

        int next = (s->tail + 1) % SOCK_QUEUE;
        if (next == s->head) return 1;   // queue full: the reply is dropped, not the oldest

        struct sock_msg *m = &s->q[s->tail];
        uint32_t plen = len - sizeof h;
        if (plen > SOCK_MSG_MAX) plen = SOCK_MSG_MAX;
        m->src = src_ip;
        m->len = plen;
        if (plen) k_memcpy(m->data, data + sizeof h, plen);
        s->tail = next;
        return 1;
    }
    return 0;
}
