// The socket table behind SYS_SOCKET/SYS_BIND/SYS_SENDTO/SYS_RECVFROM.
//
// TWO PROTOCOLS, AND THEY DEMULTIPLEX DIFFERENTLY. UDP is demuxed on a
// PORT, which is what ports are for. ICMP has no ports, so the echo
// IDENTIFIER stands in as one -- the kernel assigns it, remembers it,
// and matches replies against it, which is what lets two `ping`s run at
// once. One table serves both because the difference is one field's
// meaning, not a different object.
//
// AN ICMP SOCKET IS A PING SOCKET, NOT A RAW ONE: the kernel owns the
// header, an app sends and receives payload. That is Linux's
// IPPROTO_ICMP datagram socket, and the reason for it here is that a
// raw socket lets a process emit any ICMP type it likes -- which Linux
// gates behind CAP_NET_RAW and this kernel has no privilege model to
// gate with. A UDP socket is an ordinary one: the app owns the payload
// and the kernel owns only the header it must own.
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
#define SOCK_QUEUE    4      // datagrams held per socket

// ICMP identifiers start high enough to be recognisable in a packet
// capture and low enough to stay clear of anything a real host uses.
#define SOCK_ID_BASE  0x7001

struct sock_msg {
    uint32_t src;
    uint16_t port;    // UDP: the sender's. Zero for ICMP.
    uint32_t len;
    uint8_t data[SOCK_MSG_MAX];
};

struct socket {
    uint8_t in_use;
    uint8_t proto;          // IP_PROTO_ICMP or IP_PROTO_UDP
    uint16_t id;            // ICMP: the echo identifier
    uint16_t seq;           // ICMP: the last sequence actually sent
    uint32_t local_addr;    // UDP: 0 means any
    uint16_t local_port;    // UDP: 0 means unbound
    char dev[NET_NAME_MAX]; // bound device, empty for any
    struct sock_msg q[SOCK_QUEUE];
    int head, tail;   // tail == head means empty; one slot is left unused
};

static struct socket g_socks[SOCK_MAX];
static uint16_t g_next_ephemeral = NET_PORT_EPHEMERAL_LO;

struct icmp_echo {
    uint8_t type, code;
    uint16_t checksum;
    uint16_t id, seq;
} __attribute__((packed));

static uint8_t g_out[sizeof(struct icmp_echo) + SOCK_MSG_MAX];

static struct socket *sock_at(int sock) {
    if (sock < 0 || sock >= SOCK_MAX || !g_socks[sock].in_use) return 0;
    return &g_socks[sock];
}

// One datagram onto a socket's queue. Full means the NEW one is
// dropped, never the oldest: a queue that discards what it already
// accepted turns a burst into a silent reorder.
static int queue_push(struct socket *s, uint32_t src, uint16_t port,
                      const uint8_t *data, uint32_t len) {
    int next = (s->tail + 1) % SOCK_QUEUE;
    if (next == s->head) return 0;
    struct sock_msg *m = &s->q[s->tail];
    if (len > SOCK_MSG_MAX) len = SOCK_MSG_MAX;
    m->src = src;
    m->port = port;
    m->len = len;
    if (len) k_memcpy(m->data, data, len);
    s->tail = next;
    return 1;
}

static int port_taken(uint16_t port, int except) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (i == except || !g_socks[i].in_use) continue;
        if (g_socks[i].proto == IP_PROTO_UDP && g_socks[i].local_port == port) return 1;
    }
    return 0;
}

// The next free ephemeral port, walking the range once. Sequential
// rather than random: this kernel's randomness is a boot-time question
// (krandom's quality varies), and a predictable local port is not a
// vulnerability in a stack with no TCP sequence numbers to guess.
static uint16_t ephemeral_port(int except) {
    for (int tries = 0; tries <= NET_PORT_EPHEMERAL_HI - NET_PORT_EPHEMERAL_LO; tries++) {
        uint16_t port = g_next_ephemeral;
        g_next_ephemeral = (g_next_ephemeral >= NET_PORT_EPHEMERAL_HI)
                           ? NET_PORT_EPHEMERAL_LO : (uint16_t)(g_next_ephemeral + 1);
        if (!port_taken(port, except)) return port;
    }
    return 0;
}

int net_sock_open(int domain, int type, int protocol) {
    if (domain != NET_AF_INET || type != NET_SOCK_DGRAM) return -EINVAL;
    if (protocol != IP_PROTO_ICMP && protocol != IP_PROTO_UDP) return -EINVAL;

    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].in_use) continue;
        k_memset(&g_socks[i], 0, sizeof g_socks[i]);
        g_socks[i].in_use = 1;
        g_socks[i].proto = (uint8_t)protocol;
        g_socks[i].id = (uint16_t)(SOCK_ID_BASE + i);
        return i;
    }
    return -ENOSPC;
}

void net_sock_close(int sock) {
    if (sock < 0 || sock >= SOCK_MAX) return;
    k_memset(&g_socks[sock], 0, sizeof g_socks[sock]);
}

int net_sock_bind(int sock, uint32_t addr, uint16_t port, const char *dev) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    // An ICMP socket has no port to bind: its demux key is the
    // identifier, which the kernel assigned when the socket was
    // opened. Refusing is better than accepting and ignoring, which
    // would leave a caller believing it had reserved something.
    if (s->proto != IP_PROTO_UDP) return -EINVAL;

    if (dev && dev[0] && !net_device_by_name(dev)) return -ENODEV;

    if (port) {
        if (port_taken(port, sock)) return -EBUSY;
    } else {
        port = ephemeral_port(sock);
        if (!port) return -ENOSPC;
    }

    s->local_addr = addr;
    s->local_port = port;
    if (dev) k_strlcpy(s->dev, dev, sizeof s->dev);
    else s->dev[0] = 0;
    return port;
}

static int send_icmp(struct socket *s, uint32_t dst_ip, const void *buf, uint32_t len) {
    // THE SEQUENCE ADVANCES ONLY WHEN A PACKET REALLY LEAVES. A send
    // that comes back -EAGAIN is an ARP resolution in progress, and the
    // caller retries the SAME request -- counting those would make the
    // numbers a record of attempts rather than of packets, so a capture
    // shows a ping starting at 4 and every seq skipping unpredictably.
    uint16_t seq = (uint16_t)(s->seq + 1);

    struct icmp_echo *h = (struct icmp_echo *)g_out;
    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->checksum = 0;
    h->id = net_htons(s->id);
    h->seq = net_htons(seq);
    if (len) k_memcpy(g_out + sizeof *h, buf, len);

    uint32_t total = (uint32_t)sizeof *h + len;
    h->checksum = net_htons(net_checksum(g_out, total));

    int rc = ipv4_output(0, dst_ip, IP_PROTO_ICMP, g_out, total);
    if (rc < 0) return rc;
    s->seq = seq;
    return (int)len;
}

int net_sock_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
                    const void *buf, uint32_t len) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (len > SOCK_MSG_MAX) return -EINVAL;
    if (!dst_ip) return -EINVAL;

    if (s->proto == IP_PROTO_ICMP) return send_icmp(s, dst_ip, buf, len);

    if (!dst_port) return -EINVAL;
    // An unbound sender gets a port here rather than at open time, so a
    // client never has to call bind at all -- which is what keeps the
    // DHCP and DNS clients short. A server binds explicitly because it
    // needs a port somebody else already knows.
    if (!s->local_port) {
        s->local_port = ephemeral_port(sock);
        if (!s->local_port) return -ENOSPC;
    }

    struct net_device *dev = s->dev[0] ? net_device_by_name(s->dev) : 0;
    int rc = udp_output(dev, dst_ip, dst_port, s->local_port, buf, len);
    return rc < 0 ? rc : (int)len;
}

int net_sock_recvfrom(int sock, void *buf, uint32_t cap,
                      uint32_t *out_src, uint16_t *out_port) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (s->head == s->tail) return 0;   // nothing queued; never blocks

    struct sock_msg *m = &s->q[s->head];
    uint32_t n = m->len < cap ? m->len : cap;
    if (buf && n) k_memcpy(buf, m->data, n);
    if (out_src) *out_src = m->src;
    if (out_port) *out_port = m->port;
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
        if (!s->in_use || s->proto != IP_PROTO_ICMP || s->id != id) continue;
        queue_push(s, src_ip, 0, data + sizeof h, len - (uint32_t)sizeof h);
        return 1;
    }
    return 0;
}

int net_sock_deliver_udp(struct net_device *dev, uint32_t src_ip, uint16_t src_port,
                         uint16_t dst_port, const uint8_t *data, uint32_t len) {
    for (int i = 0; i < SOCK_MAX; i++) {
        struct socket *s = &g_socks[i];
        if (!s->in_use || s->proto != IP_PROTO_UDP) continue;
        if (s->local_port != dst_port) continue;
        // A socket bound to one card does not hear another's traffic --
        // which is the point of binding to a device, and what keeps two
        // DHCP clients on two cards from answering each other's offers.
        if (s->dev[0] && dev && k_strcmp(s->dev, dev->name) != 0) continue;
        queue_push(s, src_ip, src_port, data, len);
        return 1;
    }
    return 0;   // no listener: the caller sends a port-unreachable report
}
