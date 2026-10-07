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
#include "conn_log.h"
#include "string.h"
#include "errno.h"
#include "heap.h"
#include "scheduler.h"   // the queue's preemption guard
#include "kslots.h"

#define SOCK_MSG_MAX  SYS_NET_MSG_MAX
#define SOCK_QUEUE    4      // datagrams held per socket, one slot unused
// Payload bytes a socket may hold queued -- Linux's SO_RCVBUF, which
// counts bytes because datagrams now range from 1 byte to 64 KiB. The
// FIRST datagram into an empty queue is always taken, so one maximal
// datagram fits whatever this says.
#define SOCK_RCVBUF   (64u * 1024u)

// ICMP identifiers start high enough to be recognisable in a packet
// capture and low enough to stay clear of anything a real host uses.
#define SOCK_ID_BASE  0x7001

struct sock_msg {
    uint32_t src;
    uint16_t port;    // UDP: the sender's. Zero for ICMP.
    uint32_t len;
    uint8_t *data;    // kmalloc'd, exactly `len`; freed when read or closed
};

struct socket {
    uint8_t proto;          // IP_PROTO_ICMP or IP_PROTO_UDP
    uint16_t id;            // ICMP: the echo identifier
    uint16_t seq;           // ICMP: the last sequence actually sent
    int tcp;                // a stream socket's connection block, else -1
    uint64_t deadline_ns;   // a blocked receive's ceiling; 0 = none
    uint32_t local_addr;    // UDP: 0 means any
    uint16_t local_port;    // UDP: 0 means unbound
    // The destination this socket last made a connection-log record
    // for. ONE SLOT, not a set: it turns a datagram stream into one
    // record per destination, which is what makes a UDP flow look like
    // a connection. A socket alternating between two servers logs each
    // switch, and that is the honest cost of not carrying a table.
    uint32_t logged_ip;
    uint16_t logged_port;
    char dev[NET_NAME_MAX]; // bound device, empty for any
    struct sock_msg q[SOCK_QUEUE];
    int head, tail;   // tail == head means empty; one slot is left unused
    uint32_t queued;  // payload bytes in q[], against SOCK_RCVBUF
};

// GROWN ON DEMAND (kslots.h). A closed socket's memory stays valid, so
// net_poll() preempted while delivering to one reads a dead socket, not
// freed memory -- the property the fixed table had for free.
static struct kslots g_socks = KSLOTS_INIT(sizeof(struct socket));
static uint16_t g_next_ephemeral = NET_PORT_EPHEMERAL_LO;

struct icmp_echo {
    uint8_t type, code;
    uint16_t checksum;
    uint16_t id, seq;
} __attribute__((packed));

static struct socket *sock_at(int sock) {
    return kslots_at(&g_socks, sock);
}

// One datagram onto a socket's queue. Full means the NEW one is
// dropped, never the oldest: a queue that discards what it already
// accepted turns a burst into a silent reorder.
//
// Under the preemption guard, as its reader is: net_poll() pushes while
// a preempted recvfrom() may be half way through a pop, and `queued` is
// shared between them.
static int queue_push(struct socket *s, uint32_t src, uint16_t port,
                      const uint8_t *data, uint32_t len) {
    if (len > SOCK_MSG_MAX) return 0;
    scheduler_preempt_disable();
    int next = (s->tail + 1) % SOCK_QUEUE;
    int full = next == s->head ||
               (s->head != s->tail && s->queued + len > SOCK_RCVBUF);
    uint8_t *copy = full ? 0 : kmalloc(len ? len : 1);
    if (copy) {
        struct sock_msg *m = &s->q[s->tail];
        m->src = src;
        m->port = port;
        m->len = len;
        m->data = copy;
        if (len) k_memcpy(copy, data, len);
        s->queued += len;
        s->tail = next;
    }
    scheduler_preempt_enable();
    return copy != 0;
}

// The oldest datagram off the queue, copied out and freed. 0 when empty.
static int queue_pop(struct socket *s, void *buf, uint32_t cap,
                     uint32_t *out_src, uint16_t *out_port) {
    scheduler_preempt_disable();
    if (s->head == s->tail) { scheduler_preempt_enable(); return 0; }
    struct sock_msg *m = &s->q[s->head];
    uint32_t n = m->len < cap ? m->len : cap;
    if (buf && n) k_memcpy(buf, m->data, n);
    if (out_src) *out_src = m->src;
    if (out_port) *out_port = m->port;
    s->queued -= m->len;
    kfree(m->data);
    m->data = 0;
    s->head = (s->head + 1) % SOCK_QUEUE;
    scheduler_preempt_enable();
    return (int)n;
}

static void queue_drain(struct socket *s) {
    while (s->head != s->tail) queue_pop(s, 0, 0, 0, 0);
}

// A port is taken per PROTOCOL: UDP 80 and TCP 80 are different ports,
// as they are everywhere. An ICMP socket has no port at all and is
// skipped rather than compared.
static int port_taken(uint8_t proto, uint16_t port, int except) {
    for (int i = 0; i < kslots_cap(&g_socks); i++) {
        struct socket *s = sock_at(i);
        if (i == except || !s || s->proto != proto) continue;
        if (s->proto == IP_PROTO_ICMP) continue;
        if (s->local_port == port) return 1;
    }
    return 0;
}

// The next free ephemeral port, walking the range once. Sequential
// rather than random: this kernel's randomness is a boot-time question
// (krandom's quality varies), and a predictable local port is not a
// vulnerability in a stack with no TCP sequence numbers to guess.
static uint16_t ephemeral_port(uint8_t proto, int except) {
    for (int tries = 0; tries <= NET_PORT_EPHEMERAL_HI - NET_PORT_EPHEMERAL_LO; tries++) {
        uint16_t port = g_next_ephemeral;
        g_next_ephemeral = (g_next_ephemeral >= NET_PORT_EPHEMERAL_HI)
                           ? NET_PORT_EPHEMERAL_LO : (uint16_t)(g_next_ephemeral + 1);
        if (!port_taken(proto, port, except)) return port;
    }
    return 0;
}

int net_sock_open(int domain, int type, int protocol) {
    if (domain != NET_AF_INET) return -EINVAL;
    if (type == NET_SOCK_DGRAM) {
        if (protocol != IP_PROTO_ICMP && protocol != IP_PROTO_UDP) return -EINVAL;
    } else if (type == NET_SOCK_STREAM) {
        if (protocol != IP_PROTO_TCP) return -EINVAL;
    } else {
        return -EINVAL;
    }

    int i = kslots_alloc(&g_socks);   // zeroed
    if (i < 0) return -ENOMEM;
    struct socket *s = sock_at(i);
    s->proto = (uint8_t)protocol;
    s->id = (uint16_t)(SOCK_ID_BASE + i);
    s->tcp = -1;
    return i;
}

void net_sock_close(int sock) {
    struct socket *s = sock_at(sock);
    if (!s) return;
    // A stream is CLOSED, not abandoned: the peer is owed a FIN, and
    // tcp_close() keeps the connection block alive long enough to send
    // it and see it acknowledged.
    if (s->tcp >= 0) tcp_close(s->tcp);
    queue_drain(s);
    k_memset(s, 0, sizeof *s);
    s->tcp = -1;
    kslots_free(&g_socks, sock);
}

int net_sock_is_stream(int sock) {
    struct socket *s = sock_at(sock);
    return s && s->proto == IP_PROTO_TCP;
}

int net_sock_connect(int sock, uint32_t ip, uint16_t port) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (s->proto != IP_PROTO_TCP) return -EINVAL;
    if (s->tcp >= 0) return -EBUSY;
    if (!ip || !port) return -EINVAL;

    if (!s->local_port) {
        s->local_port = ephemeral_port(s->proto, sock);
        if (!s->local_port) return -ENOSPC;
    }
    int idx = tcp_open(s->local_port);
    if (idx < 0) return idx;
    int rc = tcp_connect(idx, ip, port);
    if (rc < 0) { tcp_release(idx); return rc; }
    s->tcp = idx;
    // LOGGED AT THE OPEN, not when the handshake finishes: a SYN that
    // nothing answers is exactly the connection somebody reading this
    // log is looking for.
    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_TCP, ip, port, s->local_port);
    return 0;
}

// Where the handshake has got to: 0 established, -EAGAIN still trying,
// or the error that ended it.
int net_sock_connect_state(int sock) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (s->tcp < 0) return -ENOTCONN;
    int err = tcp_error(s->tcp);
    if (err) return err;
    return tcp_state(s->tcp) == TCP_STATE_ESTABLISHED ? 0 : -EAGAIN;
}

int net_sock_listen(int sock) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (s->proto != IP_PROTO_TCP) return -EINVAL;
    if (s->tcp >= 0) return -EBUSY;
    // A listener must have been BOUND: a port the kernel picked is one
    // no client could know to connect to.
    if (!s->local_port) return -EINVAL;

    int idx = tcp_open(s->local_port);
    if (idx < 0) return idx;
    int rc = tcp_listen(idx);
    if (rc < 0) { tcp_release(idx); return rc; }
    s->tcp = idx;
    return 0;
}

// A finished connection, wrapped in a socket of its own -- which is
// what accept() means and why it returns a NEW descriptor. -EAGAIN
// while none is waiting.
int net_sock_accept(int sock) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (s->tcp < 0) return -EINVAL;

    int conn = tcp_accept(s->tcp);
    if (conn < 0) return conn;

    int i = kslots_alloc(&g_socks);   // `s` stays valid: objects never move
    if (i >= 0) {
        struct socket *n = sock_at(i);
        n->proto = IP_PROTO_TCP;
        n->id = (uint16_t)(SOCK_ID_BASE + i);
        n->local_port = s->local_port;
        n->tcp = conn;

        uint32_t peer_ip = 0;
        uint16_t peer_port = 0;
        tcp_peer(conn, &peer_ip, &peer_port);
        conn_log_record(QUERY_CONNLOG_IN, IP_PROTO_TCP, peer_ip, peer_port,
                        s->local_port);
        return i;
    }
    // No socket to put it in. The connection is handshaken and the peer
    // believes it is open, so it is CLOSED properly rather than
    // dropped -- otherwise the client waits out its own timeout on a
    // connection this machine has silently forgotten.
    tcp_close(conn);
    return -ENOMEM;
}

void net_sock_peer(int sock, uint32_t *out_ip, uint16_t *out_port) {
    struct socket *s = sock_at(sock);
    if (s && s->tcp >= 0) tcp_peer(s->tcp, out_ip, out_port);
}

int net_sock_stream_send(int sock, const void *buf, uint32_t len) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (s->tcp < 0) return -ENOTCONN;
    return tcp_send(s->tcp, buf, len);
}

int net_sock_stream_recv(int sock, void *buf, uint32_t cap) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    if (s->tcp < 0) return -ENOTCONN;
    return tcp_recv(s->tcp, buf, cap);
}

int net_sock_bind(int sock, uint32_t addr, uint16_t port, const char *dev) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    // An ICMP socket has no port to bind: its demux key is the
    // identifier, which the kernel assigned when the socket was
    // opened. Refusing is better than accepting and ignoring, which
    // would leave a caller believing it had reserved something. UDP and
    // TCP both bind -- a TCP listener MUST, since a port the kernel
    // picked is one no client could know to connect to.
    if (s->proto != IP_PROTO_UDP && s->proto != IP_PROTO_TCP) return -EINVAL;

    if (dev && dev[0] && !net_device_by_name(dev)) return -ENODEV;

    if (port) {
        if (port_taken(s->proto, port, sock)) return -EBUSY;
    } else {
        port = ephemeral_port(s->proto, sock);
        if (!port) return -ENOSPC;
    }

    s->local_addr = addr;
    s->local_port = port;
    if (dev) k_strlcpy(s->dev, dev, sizeof s->dev);
    else s->dev[0] = 0;
    return port;
}

// A DATAGRAM SOCKET'S FIRST SEND TO A DESTINATION IS A CONNECTION.
// Only ever called after a send actually left: an -EAGAIN is an ARP
// resolution in progress and the caller retries the same request, so
// logging the attempt would record one "connection" per retry.
static void log_flow(struct socket *s, uint8_t proto, uint32_t ip, uint16_t port) {
    if (s->logged_ip == ip && s->logged_port == port) return;
    s->logged_ip = ip;
    s->logged_port = port;
    conn_log_record(QUERY_CONNLOG_OUT, proto, ip, port, s->local_port);
}

static int send_icmp(struct socket *s, uint32_t dst_ip, const void *buf, uint32_t len) {
    // THE SEQUENCE ADVANCES ONLY WHEN A PACKET REALLY LEAVES. A send
    // that comes back -EAGAIN is an ARP resolution in progress, and the
    // caller retries the SAME request -- counting those would make the
    // numbers a record of attempts rather than of packets, so a capture
    // shows a ping starting at 4 and every seq skipping unpredictably.
    uint16_t seq = (uint16_t)(s->seq + 1);

    uint32_t total = (uint32_t)sizeof(struct icmp_echo) + len;
    uint8_t *out = kmalloc(total);
    if (!out) return -ENOMEM;
    struct icmp_echo *h = (struct icmp_echo *)out;
    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->checksum = 0;
    h->id = net_htons(s->id);
    h->seq = net_htons(seq);
    if (len) k_memcpy(out + sizeof *h, buf, len);
    h->checksum = net_htons(net_checksum(out, total));

    int rc = ipv4_output(0, dst_ip, IP_PROTO_ICMP, out, total);
    kfree(out);
    if (rc < 0) return rc;
    s->seq = seq;
    log_flow(s, IP_PROTO_ICMP, dst_ip, 0);
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
        s->local_port = ephemeral_port(s->proto, sock);
        if (!s->local_port) return -ENOSPC;
    }

    struct net_device *dev = s->dev[0] ? net_device_by_name(s->dev) : 0;
    int rc = udp_output(dev, dst_ip, dst_port, s->local_port, buf, len);
    if (rc < 0) return rc;
    log_flow(s, IP_PROTO_UDP, dst_ip, dst_port);
    return (int)len;
}

int net_sock_recvfrom(int sock, void *buf, uint32_t cap,
                      uint32_t *out_src, uint16_t *out_port) {
    struct socket *s = sock_at(sock);
    if (!s) return -EBADF;
    return queue_pop(s, buf, cap, out_src, out_port);   // never blocks
}

uint64_t net_wait_deadline(uint64_t caller_deadline) {
    uint64_t tcp = tcp_next_deadline();
    if (!tcp) return caller_deadline;
    if (!caller_deadline) return tcp;
    return tcp < caller_deadline ? tcp : caller_deadline;
}

uint64_t net_sock_deadline(int sock) {
    struct socket *s = sock_at(sock);
    return s ? s->deadline_ns : 0;
}

void net_sock_set_deadline(int sock, uint64_t ns) {
    struct socket *s = sock_at(sock);
    if (s) s->deadline_ns = ns;
}

int net_sock_deliver(uint8_t proto, uint32_t src_ip, const uint8_t *data, uint32_t len) {
    if (proto != IP_PROTO_ICMP || len < sizeof(struct icmp_echo)) return 0;

    struct icmp_echo h;
    k_memcpy(&h, data, sizeof h);
    uint16_t id = net_ntohs(h.id);

    for (int i = 0; i < kslots_cap(&g_socks); i++) {
        struct socket *s = sock_at(i);
        if (!s || s->proto != IP_PROTO_ICMP || s->id != id) continue;
        queue_push(s, src_ip, 0, data + sizeof h, len - (uint32_t)sizeof h);
        return 1;
    }
    return 0;
}

int net_sock_deliver_udp(struct net_device *dev, uint32_t src_ip, uint16_t src_port,
                         uint16_t dst_port, const uint8_t *data, uint32_t len) {
    for (int i = 0; i < kslots_cap(&g_socks); i++) {
        struct socket *s = sock_at(i);
        if (!s || s->proto != IP_PROTO_UDP) continue;
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
