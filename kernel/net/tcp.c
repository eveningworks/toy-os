// TCP, client side: an active open, an in-order byte stream, and
// retransmission. What a socket needs to fetch something.
//
// OUT-OF-ORDER SEGMENTS ARE HELD, NOT DROPPED, and they are held in
// the receive buffer itself: the window advertised is the buffer's free
// space, so a byte the window admits always has an offset to sit at.
// `ofo` names the ranges filled ahead of rcv_nxt; nothing else is
// allocated. What does NOT fit -- a range list already full, or a
// sequence past the window -- is dropped and re-acked, as before.
//
// WHAT THIS DELIBERATELY IS NOT. No SACK, so a hole costs the peer a
// retransmit timeout or three duplicate ACKs to notice; no window
// scaling, no timestamps, no RTT estimate, no Nagle (every write goes
// out at once) and no delayed ACK (every segment is acknowledged
// immediately).
//
// THE TIMERS RIDE THE BLOCKING RECEIVE. There is no softirq here and no
// kernel thread, so nothing services a connection on its own. A socket
// with something outstanding parks with a SHORT deadline instead
// (tcp_next_deadline()), wakes, runs tcp_tick() inside net_poll(), and
// blocks again -- so retransmission is driven by the process that cares
// about it. The honest gap: a connection nobody is reading has nobody
// to wake it, and its retransmits wait for the idle loop. That is
// survivable for a client and is exactly what a server could not do.
#include "net.h"
#include "netdev.h"
#include "clocksource.h"
#include "string.h"
#include "errno.h"
#include "scheduler.h" // preemption guard -- see tcp_recv()/tcp_input()

// One listener plus several live connections, and each block carries
// 12 KiB of buffers -- so this number is 96 KiB of .bss, not a free
// choice. Four was enough for a client and is not enough for a server
// that must hold a listener and the connection it is serving while a
// second waits in the backlog.
#define TCP_MAX_CONNS 8
#define TCP_SND_BUF   4096
#define TCP_RCV_BUF   8192
// Conservative for a 1500-byte MTU: 1500 - 20 (IP) - 20 (TCP) = 1460,
// and nothing here emits options after the SYN.
#define TCP_MSS       1460
// Disjoint ranges held ahead of rcv_nxt. An 8 KiB window is under six
// segments, so alternating loss can leave at most three holes; four is
// past that without being a number anyone has to think about.
#define TCP_OFO_MAX   4
// What a peer that sends no MSS option is entitled to assume of us, and
// what we assume of it (RFC 1122).
#define TCP_MSS_DEFAULT 536

#define TCP_RTO_MIN_MS  200    // BSD's fast timer, and the floor here
#define TCP_RTO_MAX_MS  4000
#define TCP_MAX_RETRIES 6

// How long an abandoned connection is kept so its FIN can be
// acknowledged. A real stack's FIN_WAIT_2 and TIME_WAIT timers are
// minutes; this is a client with four connection blocks, and holding
// one for two minutes because a peer went away would exhaust the pool
// long before it protected anything.
#define TCP_LINGER_MS 2000

// TCP_BACKLOG is in net.h: it is a property a caller can observe.

// Flags, in the order the wire has them.
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

struct tcp_header {
    uint16_t src_port, dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  offset;      // high nibble: header words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

_Static_assert(sizeof(struct tcp_header) == 20, "TCP header is 20 bytes on the wire");

struct tcp_conn {
    uint8_t in_use;
    uint8_t state;         // TCP_STATE_*
    // A connection a LISTENER produced and nobody has accepted yet. It
    // is a full connection -- handshaken, buffering whatever the client
    // sent -- with no socket in front of it, so tcp_release() is what
    // happens if the listener closes before accepting it.
    uint8_t pending;
    uint32_t remote_ip;
    uint16_t remote_port, local_port;

    uint32_t iss, snd_una, snd_nxt;
    uint32_t irs, rcv_nxt;
    uint32_t snd_wnd;      // what the peer said it will take
    uint32_t snd_mss;      // the peer's MSS: its option, or RFC 1122's 536

    uint8_t snd[TCP_SND_BUF];
    uint32_t snd_len;      // unsent + unacked bytes, starting at snd_una
    uint8_t rcv[TCP_RCV_BUF];
    uint32_t rcv_len;      // in-order bytes a reader has not taken
    // Bytes past rcv_nxt already sitting in `rcv` at their own offset,
    // named by the sequence range each covers. The list is DISJOINT:
    // an arriving segment merges into everything it touches.
    struct { uint32_t start, end; } ofo[TCP_OFO_MAX];
    uint8_t ofo_count;
    uint8_t fin_seen;      // a FIN arrived over a hole; act on it at fin_seq
    uint32_t fin_seq;      // the sequence number that FIN occupies

    uint64_t rto_at_ns;    // 0 when nothing is outstanding
    uint32_t rto_ms;
    int retries;
    uint8_t fin_queued;    // the caller closed; send FIN once the data is out
    uint8_t peer_fin;      // the peer is done sending
    uint8_t orphan;        // the application let go; the block is the stack's now
    uint64_t linger_at_ns; // when to reclaim an orphan regardless
    uint8_t reset;         // the peer aborted, or the stack gave up
    uint8_t refused;       // the RST answered our SYN: nothing is there
};

static struct tcp_conn g_conns[TCP_MAX_CONNS];
static uint8_t g_seg[sizeof(struct tcp_header) + TCP_MSS];
static uint32_t g_isn_counter;

// --- helpers ----------------------------------------------------------

// The peer's MSS option, or TCP_MSS_DEFAULT when it sent none. The walk
// is bounded by the option length in every branch: a zero-length option
// in a hostile SYN would otherwise never advance.
static uint32_t peer_mss(const uint8_t *opt, uint32_t len) {
    for (uint32_t i = 0; i + 1 < len; ) {
        uint8_t kind = opt[i];
        if (kind == 0) break;                       // end of options
        if (kind == 1) { i++; continue; }           // no-op padding
        uint8_t olen = opt[i + 1];
        if (olen < 2 || i + olen > len) break;
        if (kind == 2 && olen == 4) {
            uint32_t m = ((uint32_t)opt[i + 2] << 8) | opt[i + 3];
            if (m < 88) m = 88;                     // absurd; RFC 1122's floor
            return m < TCP_MSS ? m : TCP_MSS;       // never above our own
        }
        i += olen;
    }
    return TCP_MSS_DEFAULT;
}

// Sequence comparison is MODULAR: the space wraps, so "later" is a
// difference with the sign bit clear, never a plain <. Getting this
// wrong works perfectly until a connection crosses 2^32.
static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

// --- out-of-order ranges ----------------------------------------------
//
// A held range's bytes are ALREADY in `rcv`, at rcv_len + (start -
// rcv_nxt). That offset survives both a read (which moves the whole
// occupied region down and drops rcv_len by the same amount) and an
// absorb (which raises rcv_len and rcv_nxt together), so nothing here
// ever copies payload -- it only moves the boundary between "in order"
// and "held".

// How far past rcv_nxt the furthest held byte sits; 0 when nothing is
// held. `rcv` is occupied up to rcv_len + this, which is what a read
// has to move rather than rcv_len alone.
static uint32_t ofo_span(const struct tcp_conn *c) {
    uint32_t span = 0;
    for (uint8_t i = 0; i < c->ofo_count; i++) {
        uint32_t end = c->ofo[i].end - c->rcv_nxt;
        if (end > span) span = end;
    }
    return span;
}

// Remember [start, end), merged into every range it touches or
// overlaps -- a segment bridging two holes collapses all three into
// one. Returns 0 when the list is full, which makes the segment a drop
// the peer will resend.
static int ofo_record(struct tcp_conn *c, uint32_t start, uint32_t end) {
    for (uint8_t i = 0; i < c->ofo_count; ) {
        if (seq_le(c->ofo[i].start, end) && seq_le(start, c->ofo[i].end)) {
            if (seq_lt(c->ofo[i].start, start)) start = c->ofo[i].start;
            if (seq_lt(end, c->ofo[i].end)) end = c->ofo[i].end;
            c->ofo[i] = c->ofo[--c->ofo_count];   // and re-test this slot
            continue;
        }
        i++;
    }
    if (c->ofo_count >= TCP_OFO_MAX) return 0;
    c->ofo[c->ofo_count].start = start;
    c->ofo[c->ofo_count].end = end;
    c->ofo_count++;
    return 1;
}

// The hole in front of the held ranges just filled: hand whatever now
// reaches rcv_nxt to the reader. A range the stream has already passed
// is discarded rather than absorbed, which is what an overlapping
// retransmission leaves behind.
static void ofo_drain(struct tcp_conn *c) {
    for (int moved = 1; moved; ) {
        moved = 0;
        for (uint8_t i = 0; i < c->ofo_count; i++) {
            if (!seq_le(c->ofo[i].start, c->rcv_nxt)) continue;
            if (seq_lt(c->rcv_nxt, c->ofo[i].end)) {
                uint32_t n = c->ofo[i].end - c->rcv_nxt;
                c->rcv_len += n;
                c->rcv_nxt += n;
            }
            c->ofo[i] = c->ofo[--c->ofo_count];
            moved = 1;
            break;
        }
    }
}

static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t len) {
    uint8_t pseudo[12];
    pseudo[0] = (uint8_t)(src >> 24); pseudo[1] = (uint8_t)(src >> 16);
    pseudo[2] = (uint8_t)(src >> 8);  pseudo[3] = (uint8_t)src;
    pseudo[4] = (uint8_t)(dst >> 24); pseudo[5] = (uint8_t)(dst >> 16);
    pseudo[6] = (uint8_t)(dst >> 8);  pseudo[7] = (uint8_t)dst;
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    pseudo[10] = (uint8_t)(len >> 8);
    pseudo[11] = (uint8_t)len;
    return net_checksum_two(pseudo, sizeof pseudo, seg, len);
}

static uint32_t local_ip_for(uint32_t dst) {
    uint32_t next = 0;
    struct net_device *d = ipv4_route(dst, &next);
    return d ? d->ip : 0;
}

// One segment onto the wire. `data`/`data_len` may be empty, which is
// what an ACK, a SYN and a FIN all are.
static int send_segment(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                        const uint8_t *data, uint32_t data_len) {
    if (data_len > TCP_MSS) data_len = TCP_MSS;

    struct tcp_header *h = (struct tcp_header *)g_seg;
    // AN MSS OPTION RIDES EVERY SYN, and leaving it off is not a missing
    // optimisation but a wrong answer: RFC 1122 requires a sender that
    // received no MSS to assume 536, so every peer sent 536-byte
    // segments -- 2.7x the frames for the same bytes.
    uint32_t optlen = 0;
    if (flags & TH_SYN) {
        uint8_t *o = g_seg + sizeof *h;
        o[0] = 2; o[1] = 4;                      // kind, length
        o[2] = (uint8_t)(TCP_MSS >> 8); o[3] = (uint8_t)TCP_MSS;
        optlen = 4;
    }
    h->src_port = net_htons(c->local_port);
    h->dst_port = net_htons(c->remote_port);
    h->seq = net_htonl(seq);
    h->ack = net_htonl(c->rcv_nxt);
    h->offset = (uint8_t)(((sizeof *h + optlen) / 4) << 4);
    h->flags = flags;
    // What we will accept: the free space in the receive buffer. A
    // window advertised larger than the buffer is how a stack loses
    // data it promised to take.
    h->window = net_htons((uint16_t)(TCP_RCV_BUF - c->rcv_len));
    h->checksum = 0;
    h->urgent = 0;
    if (data_len) k_memcpy(g_seg + sizeof *h + optlen, data, data_len);

    uint32_t total = (uint32_t)sizeof *h + optlen + data_len;
    uint32_t src = local_ip_for(c->remote_ip);
    uint16_t sum = tcp_checksum(src, c->remote_ip, g_seg, total);
    h->checksum = net_htons(sum ? sum : 0xFFFF);

    return ipv4_output(0, c->remote_ip, IP_PROTO_TCP, g_seg, total);
}

// Arm the retransmit timer if anything is outstanding, disarm it if
// not. Called wherever snd_una or the queue moves, so "is a timer
// running" is derived rather than remembered in two places.
static void arm_timer(struct tcp_conn *c) {
    int outstanding = (c->snd_nxt != c->snd_una) ||
                      c->state == TCP_STATE_SYN_SENT ||
                      c->state == TCP_STATE_SYN_RCVD ||
                      c->state == TCP_STATE_FIN_WAIT_1 ||
                      c->state == TCP_STATE_LAST_ACK;
    if (!outstanding) {
        c->rto_at_ns = 0;
        c->retries = 0;
        return;
    }
    if (!c->rto_at_ns) {
        if (!c->rto_ms) c->rto_ms = TCP_RTO_MIN_MS;
        c->rto_at_ns = clocksource_now_ns() + (uint64_t)c->rto_ms * 1000000ull;
    }
}

// Everything a connection can send right now: what the peer's window
// allows, capped at one segment. Returns the bytes put on the wire.
static uint32_t send_pending(struct tcp_conn *c) {
    if (c->state != TCP_STATE_ESTABLISHED && c->state != TCP_STATE_CLOSE_WAIT) return 0;

    uint32_t in_flight = c->snd_nxt - c->snd_una;
    if (in_flight >= c->snd_len) return 0;      // everything is out already
    uint32_t ready = c->snd_len - in_flight;
    uint32_t window = c->snd_wnd > in_flight ? c->snd_wnd - in_flight : 0;
    if (!window) return 0;                       // the peer is full; the timer probes
    uint32_t n = ready < window ? ready : window;
    if (n > c->snd_mss) n = c->snd_mss;

    if (send_segment(c, TH_ACK | TH_PSH, c->snd_nxt, c->snd + in_flight, n) < 0) return 0;
    c->snd_nxt += n;
    arm_timer(c);
    return n;
}

// The caller closed and the data is out: FIN goes after it, never
// before -- a FIN sent while bytes are still queued would end the
// stream at the wrong place.
static void maybe_send_fin(struct tcp_conn *c) {
    if (!c->fin_queued) return;
    if (c->snd_nxt != c->snd_una + c->snd_len) return;   // data still in flight
    if (c->state == TCP_STATE_ESTABLISHED) {
        send_segment(c, TH_ACK | TH_FIN, c->snd_nxt, 0, 0);
        c->snd_nxt++;
        c->state = TCP_STATE_FIN_WAIT_1;
        arm_timer(c);
    } else if (c->state == TCP_STATE_CLOSE_WAIT) {
        send_segment(c, TH_ACK | TH_FIN, c->snd_nxt, 0, 0);
        c->snd_nxt++;
        c->state = TCP_STATE_LAST_ACK;
        arm_timer(c);
    }
}

// --- the public half --------------------------------------------------

int tcp_open(uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (g_conns[i].in_use) continue;
        k_memset(&g_conns[i], 0, sizeof g_conns[i]);
        g_conns[i].in_use = 1;
        g_conns[i].state = TCP_STATE_CLOSED;
        g_conns[i].local_port = local_port;
        return i;
    }
    return -ENOSPC;
}

void tcp_release(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNS) return;
    k_memset(&g_conns[idx], 0, sizeof g_conns[idx]);
}

int tcp_state(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return TCP_STATE_CLOSED;
    return g_conns[idx].state;
}

// Why a connection is not usable, or 0 while it still might be. The
// handshake's own result: a caller polls this rather than tcp_state(),
// because CLOSED is both "not started" and "refused".
void tcp_peer(int idx, uint32_t *out_ip, uint16_t *out_port) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return;
    if (out_ip) *out_ip = g_conns[idx].remote_ip;
    if (out_port) *out_port = g_conns[idx].remote_port;
}

int tcp_error(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return -EBADF;
    if (g_conns[idx].refused) return -ECONNREFUSED;
    if (g_conns[idx].reset) return -ECONNRESET;
    return 0;
}

int tcp_connect(int idx, uint32_t ip, uint16_t port) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return -EBADF;
    struct tcp_conn *c = &g_conns[idx];
    if (c->state != TCP_STATE_CLOSED) return -EBUSY;
    if (!local_ip_for(ip)) return -ENODEV;

    // The initial sequence number is not random here, and that is worth
    // being explicit about: RFC 6528 wants it unpredictable to make
    // off-path injection hard, and this kernel has no threat model in
    // which that matters yet. It advances per connection so two
    // connections in a row do not reuse a number.
    c->iss = (g_isn_counter += 0x01000193u) + (uint32_t)clocksource_now_ns();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;      // SYN takes a sequence number
    c->remote_ip = ip;
    c->remote_port = port;
    c->snd_wnd = TCP_MSS;         // until the peer tells us otherwise
    c->snd_mss = TCP_MSS_DEFAULT; // ditto, and RFC 1122 names the value
    c->state = TCP_STATE_SYN_SENT;
    c->rto_ms = TCP_RTO_MIN_MS;
    c->rto_at_ns = 0;

    int rc = send_segment(c, TH_SYN, c->iss, 0, 0);
    if (rc < 0 && rc != -EAGAIN) {   // -EAGAIN is ARP; the timer retries
        c->state = TCP_STATE_CLOSED;
        return rc;
    }
    arm_timer(c);
    return 0;
}

int tcp_listen(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return -EBADF;
    struct tcp_conn *c = &g_conns[idx];
    if (c->state != TCP_STATE_CLOSED) return -EBUSY;
    if (!c->local_port) return -EINVAL;      // nothing to listen ON
    c->state = TCP_STATE_LISTEN;
    return 0;
}

// The first finished connection this listener produced, or -EAGAIN.
// The caller wraps it in a socket of its own; from here on the two
// blocks are unrelated.
int tcp_accept(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return -EBADF;
    if (g_conns[idx].state != TCP_STATE_LISTEN) return -EINVAL;

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *k = &g_conns[i];
        if (!k->in_use || !k->pending) continue;
        if (k->local_port != g_conns[idx].local_port) continue;
        if (k->state != TCP_STATE_ESTABLISHED && k->state != TCP_STATE_CLOSE_WAIT) continue;
        k->pending = 0;      // it has an owner now
        return i;
    }
    return -EAGAIN;
}

int tcp_send(int idx, const void *buf, uint32_t len) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return -EBADF;
    struct tcp_conn *c = &g_conns[idx];
    if (c->refused) return -ECONNREFUSED;
    if (c->reset) return -ECONNRESET;
    if (c->state != TCP_STATE_ESTABLISHED && c->state != TCP_STATE_CLOSE_WAIT)
        return -ENOTCONN;
    if (c->fin_queued) return -EPIPE;

    uint32_t room = TCP_SND_BUF - c->snd_len;
    if (!room) return -EAGAIN;          // the buffer is full; drain it first
    uint32_t n = len < room ? len : room;
    k_memcpy(c->snd + c->snd_len, buf, n);
    c->snd_len += n;
    send_pending(c);
    return (int)n;
}

int tcp_recv(int idx, void *buf, uint32_t cap) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return -EBADF;
    struct tcp_conn *c = &g_conns[idx];

    if (c->rcv_len) {
        // THE RECEIVE BUFFER IS NOT RE-ENTRANT, and this is the same
        // guard `vfs.c` holds for the same reason. A ring-3 process is
        // preemptible inside a syscall, and `net_poll()` -- which runs
        // `tcp_input()` into THIS buffer -- is reached from
        // scheduler_idle() and from a dozen syscalls. Preempted between
        // the copy-out and the compaction, a reader resumes and shifts
        // a segment that arrived meanwhile: one MSS of the stream comes
        // out holding bytes from a few hundred bytes earlier, and
        // nothing reports anything.
        scheduler_preempt_disable();
        uint32_t n = c->rcv_len < cap ? c->rcv_len : cap;
        k_memcpy(buf, c->rcv, n);
        // A ring would avoid this move; a linear buffer plus a memmove
        // is chosen because the reader almost always takes everything,
        // which makes the move free and the code obviously correct.
        // HELD RANGES MOVE WITH IT -- they live past rcv_len in the same
        // buffer, and leaving them behind would deliver them shifted.
        uint32_t occupied = c->rcv_len + ofo_span(c);
        if (n < occupied) {
            for (uint32_t i = 0; i < occupied - n; i++) c->rcv[i] = c->rcv[n + i];
        }
        c->rcv_len -= n;
        scheduler_preempt_enable();
        // The window just opened. Telling the peer costs one segment
        // and is what stops a transfer stalling at a closed window.
        send_segment(c, TH_ACK, c->snd_nxt, 0, 0);
        return (int)n;
    }
    if (c->refused) return -ECONNREFUSED;
    if (c->reset) return -ECONNRESET;
    if (c->peer_fin) return 0;          // orderly end of stream
    if (c->state == TCP_STATE_CLOSED) return -ENOTCONN;
    return -EAGAIN;                     // nothing yet -- the caller waits
}

void tcp_close(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNS || !g_conns[idx].in_use) return;
    struct tcp_conn *c = &g_conns[idx];
    if (c->state == TCP_STATE_SYN_SENT || c->state == TCP_STATE_CLOSED) {
        tcp_release(idx);
        return;
    }
    // THE BLOCK OUTLIVES THE SOCKET, and something has to reclaim it.
    // The application is gone, so nothing will ever call back in: the
    // FIN still has to be sent and acknowledged, and if the peer never
    // answers, the linger deadline is what stops four dead connections
    // holding the whole pool until reboot.
    c->orphan = 1;
    c->linger_at_ns = clocksource_now_ns() + (uint64_t)TCP_LINGER_MS * 1000000ull;
    c->fin_queued = 1;
    maybe_send_fin(c);
}

// The earliest deadline any connection is working to, so a blocked
// reader can park until then instead of waiting out its own timeout.
// 0 means nothing is outstanding anywhere. Lingering orphans count:
// nobody is reading them, so the next reader to park is what reclaims
// them.
uint64_t tcp_next_deadline(void) {
    uint64_t best = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!g_conns[i].in_use) continue;
        uint64_t when = g_conns[i].rto_at_ns;
        if (g_conns[i].orphan && (!when || g_conns[i].linger_at_ns < when))
            when = g_conns[i].linger_at_ns;
        if (!when) continue;
        if (!best || when < best) best = when;
    }
    return best;
}

// Retransmission, run from net_poll(). Everything here is what the
// process that woke up owes its connection.
void tcp_tick(void) {
    uint64_t now = clocksource_now_ns();
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->in_use) continue;

        // An abandoned connection is reclaimed as soon as it is really
        // closed, or when its linger expires -- whichever comes first.
        if (c->orphan && (c->state == TCP_STATE_CLOSED || now >= c->linger_at_ns)) {
            tcp_release(i);
            continue;
        }

        // An -EAGAIN from ARP left the segment unsent; retrying is the
        // same act as retransmitting, so there is one path for both.
        if (!c->rto_at_ns || c->rto_at_ns > now) {
            send_pending(c);
            maybe_send_fin(c);
            continue;
        }

        if (++c->retries > TCP_MAX_RETRIES) {
            if (c->pending) { tcp_release(i); continue; }   // see backlog_depth()
            c->reset = 1;                  // gave up: report it like a reset
            c->state = TCP_STATE_CLOSED;
            c->rto_at_ns = 0;
            continue;
        }
        // Exponential backoff, capped. No RTT estimate: measuring one
        // needs a timestamp per segment, and a fixed floor with backoff
        // is what a client fetching over a local network needs.
        c->rto_ms = c->rto_ms * 2 > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : c->rto_ms * 2;
        c->rto_at_ns = now + (uint64_t)c->rto_ms * 1000000ull;

        switch (c->state) {   // dispatch-ok: bounded by the state machine
        case TCP_STATE_SYN_SENT:
            send_segment(c, TH_SYN, c->iss, 0, 0);
            break;
        case TCP_STATE_SYN_RCVD:
            send_segment(c, TH_SYN | TH_ACK, c->iss, 0, 0);
            break;
        case TCP_STATE_ESTABLISHED:
        case TCP_STATE_CLOSE_WAIT: {
            uint32_t in_flight = c->snd_nxt - c->snd_una;
            if (in_flight) send_segment(c, TH_ACK | TH_PSH, c->snd_una, c->snd, in_flight);
            break;
        }
        case TCP_STATE_FIN_WAIT_1:
        case TCP_STATE_LAST_ACK:
            send_segment(c, TH_ACK | TH_FIN, c->snd_nxt - 1, 0, 0);
            break;
        default:
            break;
        }
    }
}

// --- receive ----------------------------------------------------------

// THE FOUR-TUPLE FIRST, THE LISTENER ONLY IF NOTHING MATCHED. That
// order is the whole demultiplexing rule: a listener and the
// connections it produced all share a local port, so a lookup that
// checked the listener first would hand every segment of every live
// connection to it.
static struct tcp_conn *find_conn(uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->in_use || c->state == TCP_STATE_CLOSED) continue;
        if (c->state == TCP_STATE_LISTEN) continue;
        if (c->local_port == dst_port && c->remote_port == src_port &&
            c->remote_ip == src_ip) return c;
    }
    return 0;
}

static struct tcp_conn *find_listener(uint16_t dst_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (c->in_use && c->state == TCP_STATE_LISTEN && c->local_port == dst_port)
            return c;
    }
    return 0;
}

// A PENDING CONNECTION THAT DIES BEFORE accept() IS RELEASED WHERE IT
// DIES -- the give-up in tcp_tick() and the RST above. Nobody owns it,
// accept() only takes a live one, and backlog_depth() counts it whatever
// its state: left in place, TCP_BACKLOG dead handshakes silenced a
// listener until reboot (telnetd, 2026-09-27). Linux frees a half-open
// request the same way, from its own SYN-queue timer.
//
// How many connections a listener has produced that nobody has accepted
// yet. This IS the backlog: there is no separate queue, because a
// half-finished connection needs a full block anyway.
static int backlog_depth(uint16_t port) {
    int n = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (c->in_use && c->pending && c->local_port == port) n++;
    }
    return n;
}

// A SYN for a listening port: give it a block of its own and answer.
// The listener is untouched -- it goes on listening, and this block
// carries the connection from here.
static void passive_open(struct tcp_conn *lis, uint32_t src_ip, uint16_t src_port,
                         uint32_t seq, uint16_t window,
                         const uint8_t *opt, uint32_t optlen) {
    if (backlog_depth(lis->local_port) >= TCP_BACKLOG) {
        // FULL: say nothing. The client's own SYN retransmission brings
        // it back when a slot frees, so a brief burst succeeds slightly
        // later instead of failing -- Linux's default, and the reason
        // it is a drop rather than a RST.
        return;
    }
    int idx = tcp_open(lis->local_port);
    if (idx < 0) return;    // no block: the same silence, for the same reason

    struct tcp_conn *c = &g_conns[idx];
    c->pending = 1;
    c->remote_ip = src_ip;
    c->remote_port = src_port;
    c->irs = seq;
    c->rcv_nxt = seq + 1;
    c->iss = (g_isn_counter += 0x01000193u) + (uint32_t)clocksource_now_ns();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;    // our SYN takes a sequence number
    c->snd_wnd = window ? window : TCP_MSS;
    c->snd_mss = peer_mss(opt, optlen);
    c->state = TCP_STATE_SYN_RCVD;
    c->rto_ms = TCP_RTO_MIN_MS;

    send_segment(c, TH_SYN | TH_ACK, c->iss, 0, 0);
    arm_timer(c);
}

int tcp_input(struct net_device *dev, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t *pkt, uint32_t len) {
    (void)dev;
    if (len < sizeof(struct tcp_header)) return 1;
    if (tcp_checksum(src_ip, dst_ip, pkt, len) != 0) return 1;

    struct tcp_header h;
    k_memcpy(&h, pkt, sizeof h);
    uint32_t doff = (uint32_t)(h.offset >> 4) * 4;
    if (doff < sizeof h || doff > len) return 1;

    uint16_t sport = net_ntohs(h.src_port), dport = net_ntohs(h.dst_port);
    uint32_t seq_in = net_ntohl(h.seq);
    struct tcp_conn *c = find_conn(src_ip, sport, dport);
    if (!c) {
        // Nothing matched the four-tuple. A bare SYN for a listening
        // port opens a connection; anything else is a segment for a
        // conversation this stack is not having.
        struct tcp_conn *lis = find_listener(dport);
        if (lis && (h.flags & TH_SYN) && !(h.flags & TH_ACK)) {
            passive_open(lis, src_ip, sport, seq_in, net_ntohs(h.window),
                         pkt + sizeof h, doff - (uint32_t)sizeof h);
            return 1;
        }
        return 0;
    }

    uint32_t seq = seq_in, ack = net_ntohl(h.ack);
    const uint8_t *data = pkt + doff;
    uint32_t data_len = len - doff;

    if (h.flags & TH_RST) {
        // A RST IS VALIDATED BEFORE IT IS BELIEVED. Accepting any reset
        // that names the right ports lets anyone who can guess a
        // connection tear it down -- the blind-reset attack RFC 5961
        // exists for. In SYN_SENT the reset must acknowledge our SYN;
        // afterwards it must sit exactly where the next byte was
        // expected. (RFC 5961 accepts a window and challenges the rest;
        // that needs a challenge-ACK rate limit, which is a mechanism
        // this stack has no other use for.)
        if (c->state == TCP_STATE_SYN_SENT) {
            if (!(h.flags & TH_ACK) || ack != c->snd_nxt) return 1;
            c->refused = 1;
        } else if (seq_in != c->rcv_nxt) {
            return 1;
        }
        c->reset = 1;
        c->state = TCP_STATE_CLOSED;
        c->rto_at_ns = 0;
        if (c->pending) tcp_release((int)(c - g_conns));   // see backlog_depth()
        return 1;
    }

    if (c->state == TCP_STATE_SYN_SENT) {
        // The only acceptable answer to our SYN is SYN+ACK acking it.
        if (!(h.flags & TH_SYN) || !(h.flags & TH_ACK)) return 1;
        if (ack != c->iss + 1) return 1;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = net_ntohs(h.window);
        c->snd_mss = peer_mss(pkt + sizeof h, doff - (uint32_t)sizeof h);
        c->state = TCP_STATE_ESTABLISHED;
        c->rto_at_ns = 0;
        c->rto_ms = TCP_RTO_MIN_MS;
        c->retries = 0;
        send_segment(c, TH_ACK, c->snd_nxt, 0, 0);
        arm_timer(c);
        return 1;
    }

    if (c->state == TCP_STATE_SYN_RCVD) {
        // The third leg. Anything else at this point -- including a
        // retransmitted SYN, which arrives when our SYN+ACK was lost --
        // leaves the connection where it is for the timer to retry.
        if ((h.flags & TH_ACK) && ack == c->snd_nxt) {
            c->snd_una = ack;
            c->snd_wnd = net_ntohs(h.window);
            c->state = TCP_STATE_ESTABLISHED;
            c->rto_at_ns = 0;
            c->retries = 0;
            c->rto_ms = TCP_RTO_MIN_MS;
        } else if (!(h.flags & TH_ACK)) {
            return 1;
        }
    }

    if (h.flags & TH_ACK) {
        if (seq_le(c->snd_una, ack) && seq_le(ack, c->snd_nxt)) {
            uint32_t acked = ack - c->snd_una;
            // A FIN occupies a sequence number but no buffer byte, so
            // only the part covering real data may be consumed.
            uint32_t data_acked = acked;
            if (data_acked > c->snd_len) data_acked = c->snd_len;
            if (data_acked) {
                for (uint32_t i = 0; i < c->snd_len - data_acked; i++)
                    c->snd[i] = c->snd[data_acked + i];
                c->snd_len -= data_acked;
            }
            c->snd_una = ack;
            c->retries = 0;
            c->rto_ms = TCP_RTO_MIN_MS;
            c->rto_at_ns = 0;
            arm_timer(c);

            if (c->state == TCP_STATE_FIN_WAIT_1 && c->snd_una == c->snd_nxt)
                c->state = c->peer_fin ? TCP_STATE_CLOSED : TCP_STATE_FIN_WAIT_2;
            else if (c->state == TCP_STATE_LAST_ACK && c->snd_una == c->snd_nxt)
                c->state = TCP_STATE_CLOSED;
        }
        c->snd_wnd = net_ntohs(h.window);
    }

    // The window is the free space in `rcv`, so a byte the window admits
    // has an offset waiting for it whether or not it is the next one:
    // in-order bytes extend rcv_len, the rest are copied into place and
    // their range remembered until the hole ahead of them fills.
    int took = 0;
    uint32_t fin_seq = seq + data_len;      // where a FIN in this segment sits
    // The other half of tcp_recv()'s guard: this writes into the same
    // buffer a reader compacts, and it is equally preemptible.
    scheduler_preempt_disable();
    if (data_len) {
        const uint8_t *p = data;
        uint32_t s = seq, n = data_len;
        if (seq_lt(s, c->rcv_nxt)) {        // a prefix we already have
            uint32_t skip = c->rcv_nxt - s;
            if (skip >= n) n = 0;
            else { p += skip; s += skip; n -= skip; }
        }
        uint32_t window = TCP_RCV_BUF - c->rcv_len;
        if (n && !seq_lt(s, c->rcv_nxt + window)) n = 0;   // past what we promised
        if (n) {
            uint32_t off = c->rcv_len + (s - c->rcv_nxt);
            uint32_t room = TCP_RCV_BUF - off;
            if (n > room) n = room;
            k_memcpy(c->rcv + off, p, n);
            if (s == c->rcv_nxt) {
                c->rcv_len += n;
                c->rcv_nxt += n;
                ofo_drain(c);
                took = 1;
            } else if (ofo_record(c, s, s + n)) {
                took = 1;
            }
        }
    }

    // A FIN SITS AFTER THE SEGMENT'S LAST BYTE, so one that arrives over
    // a hole is remembered and acted on when the stream reaches it --
    // ending the stream early would hide data still on its way.
    if ((h.flags & TH_FIN) && !c->peer_fin && seq_le(c->rcv_nxt, fin_seq)) {
        c->fin_seq = fin_seq;
        c->fin_seen = 1;
    }
    scheduler_preempt_enable();

    if (c->fin_seen && c->rcv_nxt == c->fin_seq) {
        c->fin_seen = 0;
        c->peer_fin = 1;
        c->rcv_nxt++;
        took = 1;
        if (c->state == TCP_STATE_ESTABLISHED) c->state = TCP_STATE_CLOSE_WAIT;
        else if (c->state == TCP_STATE_FIN_WAIT_2) c->state = TCP_STATE_CLOSED;
        else if (c->state == TCP_STATE_FIN_WAIT_1 && c->snd_una == c->snd_nxt)
            c->state = TCP_STATE_CLOSED;
    }

    if (took || data_len) send_segment(c, TH_ACK, c->snd_nxt, 0, 0);
    send_pending(c);
    maybe_send_fin(c);
    return 1;
}
