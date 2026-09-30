// IPv4 reassembly: fragments in, one whole datagram out.
//
// A DATAGRAM IN PROGRESS IS A 64 KiB BUFFER AND A BITMAP, allocated
// when its first fragment arrives and freed when it completes or times
// out. One bit per 8-byte block, which is the unit fragment offsets are
// counted in, so "have I got this range" is a bit test rather than
// RFC 815's hole list. At most FRAG_SLOTS at once: that is the memory
// an attacker can pin by sending first fragments and nothing else.
//
// THE KEY IS (source, destination, id, protocol), RFC 791's, and not
// the device -- a datagram's fragments may arrive on different cards.
//
// A PARTIAL OVERLAP DROPS THE WHOLE DATAGRAM, as Linux has since
// FragmentSmack: which copy of an overlapped byte is "right" is exactly
// the ambiguity overlap attacks exploit, and a parser REJECTS rather
// than guesses. A fragment whose range is already COMPLETELY held is a
// duplicate, not an attack, and is ignored -- the first copy stands.
//
// EXPIRY IS LAZY: net_poll() calls ipv4_frag_expire() on every pass
// and asks for no wake of its own. A stale datagram costs its buffer
// until the next frame or socket call, which on an idle machine is
// memory nobody else wanted either.
#include "net.h"
#include "netdev.h"
#include "string.h"
#include "heap.h"
#include "kfmt.h"   // klog_printf

#define FRAG_SLOTS       4
#define FRAG_TIMEOUT_NS  (30ull * 1000000000ull)   // Linux's ipfrag_time
#define FRAG_HDR_MAX     60                        // IHL 15 * 4
// The largest payload any IHL allows. Whether this one fits under the
// 16-bit total length is checked at completion, once the IHL is known.
#define FRAG_PAYLOAD_MAX (IP_DATAGRAM_MAX - 20)
#define FRAG_BLOCKS      ((FRAG_PAYLOAD_MAX + 7) / 8)

struct frag_slot {
    uint8_t  in_use;
    uint8_t  proto;
    uint16_t id;
    uint32_t src, dst;
    struct net_device *dev;   // the first fragment's, for the timeout report
    uint64_t expires_ns;
    uint32_t total;           // payload length, known once the last fragment is in
    uint32_t got;             // payload bytes held; == total means complete
    uint32_t ihl;             // 0 until fragment zero arrives
    // The header sits right-aligned against the payload, at
    // buf + FRAG_HDR_MAX - ihl, so a complete datagram is contiguous.
    uint8_t *buf;
    uint8_t  map[(FRAG_BLOCKS + 7) / 8];
};

static struct frag_slot g_slots[FRAG_SLOTS];

static void slot_free(struct frag_slot *s) {
    if (s->buf) kfree(s->buf);
    k_memset(s, 0, sizeof *s);
}

static int block_held(const struct frag_slot *s, uint32_t b) {
    return (s->map[b >> 3] >> (b & 7)) & 1;
}

// 0 none of [first, last] held, 1 all of it, -1 some.
static int range_held(const struct frag_slot *s, uint32_t first, uint32_t last) {
    uint32_t held = 0;
    for (uint32_t b = first; b <= last; b++) held += (uint32_t)block_held(s, b);
    if (held == 0) return 0;
    return held == last - first + 1 ? 1 : -1;
}

// ICMP Time Exceeded, code 1 -- but only with fragment zero in hand
// (RFC 1122 3.3.2): the report quotes the header, and without that
// fragment there is no header to quote.
static void report_timeout(struct frag_slot *s) {
    if (!s->ihl || !block_held(s, 0) || !s->dev) return;
    const uint8_t *hdr = s->buf + FRAG_HDR_MAX - s->ihl;
    uint32_t have = s->got < 8 ? s->got : 8;
    icmp_send_error(s->dev, s->src, ICMP_TIME_EXCEEDED, ICMP_CODE_REASSEMBLY,
                    hdr, s->ihl + have);
}

void ipv4_frag_expire(uint64_t now_ns) {
    for (int i = 0; i < FRAG_SLOTS; i++) {
        struct frag_slot *s = &g_slots[i];
        if (!s->in_use || now_ns < s->expires_ns) continue;
        // At most FRAG_SLOTS lines a timeout period, so it cannot flood.
        klog_printf("ipv4: reassembly timed out -- id %u from %u.%u.%u.%u, "
                    "%u bytes held of %u%s\n", s->id,
                    s->src >> 24, (s->src >> 16) & 0xFF, (s->src >> 8) & 0xFF,
                    s->src & 0xFF, s->got, s->total,
                    s->total ? "" : " (last fragment never came)");
        report_timeout(s);
        slot_free(s);
    }
}

int ipv4_frag_pending(void) {
    int n = 0;
    for (int i = 0; i < FRAG_SLOTS; i++) n += g_slots[i].in_use;
    return n;
}

void ipv4_frag_flush(void) {
    for (int i = 0; i < FRAG_SLOTS; i++)
        if (g_slots[i].in_use) slot_free(&g_slots[i]);
}

// The matching slot, or a fresh one. When all are busy the OLDEST is
// evicted (Linux's LRU evictor): refusing the newcomer instead would
// let four stalled datagrams block every other one for 30 seconds.
static struct frag_slot *slot_for(uint32_t src, uint32_t dst, uint16_t id,
                                  uint8_t proto, uint64_t now) {
    struct frag_slot *free_slot = 0, *oldest = 0;
    for (int i = 0; i < FRAG_SLOTS; i++) {
        struct frag_slot *s = &g_slots[i];
        if (!s->in_use) { if (!free_slot) free_slot = s; continue; }
        if (s->src == src && s->dst == dst && s->id == id && s->proto == proto) return s;
        if (!oldest || s->expires_ns < oldest->expires_ns) oldest = s;
    }
    struct frag_slot *s = free_slot;
    if (!s) { s = oldest; slot_free(s); }

    s->buf = kmalloc(FRAG_HDR_MAX + FRAG_PAYLOAD_MAX);
    if (!s->buf) return 0;
    s->in_use = 1;
    s->src = src; s->dst = dst; s->id = id; s->proto = proto;
    s->expires_ns = now + FRAG_TIMEOUT_NS;
    return s;
}

void ipv4_frag_input(struct net_device *dev, const uint8_t *pkt,
                     uint32_t ihl, uint32_t total, uint64_t now_ns) {
    uint16_t flags_frag = (uint16_t)((pkt[6] << 8) | pkt[7]);
    int more = (flags_frag & IP_FLAG_MF) != 0;
    uint32_t off = (uint32_t)(flags_frag & IP_FRAG_MASK) * 8;
    uint32_t len = total - ihl;

    // Every fragment but the last carries whole blocks, or the next
    // one's offset could not start where it ends. Empty ones carry
    // nothing to place.
    if (!len || (more && (len & 7))) return;
    if (off + len > FRAG_PAYLOAD_MAX) return;

    uint32_t src = (uint32_t)pkt[12] << 24 | (uint32_t)pkt[13] << 16 |
                   (uint32_t)pkt[14] << 8  | pkt[15];
    uint32_t dst = (uint32_t)pkt[16] << 24 | (uint32_t)pkt[17] << 16 |
                   (uint32_t)pkt[18] << 8  | pkt[19];
    uint16_t id = (uint16_t)((pkt[4] << 8) | pkt[5]);

    struct frag_slot *s = slot_for(src, dst, id, pkt[9], now_ns);
    if (!s) return;

    uint32_t end = off + len;
    if (!more) {
        // A second "last" fragment naming a different end, or data
        // already held past this end, is a datagram that disagrees
        // with itself.
        if (s->total && s->total != end) { slot_free(s); return; }
        for (uint32_t b = (end + 7) / 8; b < FRAG_BLOCKS; b++)
            if (block_held(s, b)) { slot_free(s); return; }
        s->total = end;
    } else if (s->total && end > s->total) {
        slot_free(s);
        return;
    }

    uint32_t first = off / 8, last = (end - 1) / 8;
    int held = range_held(s, first, last);
    if (held == 1) return;                        // a duplicate
    if (held < 0) { slot_free(s); return; }       // a partial overlap

    for (uint32_t b = first; b <= last; b++) s->map[b >> 3] |= (uint8_t)(1u << (b & 7));
    k_memcpy(s->buf + FRAG_HDR_MAX + off, pkt + ihl, len);
    s->got += len;
    if (!s->dev) s->dev = dev;
    if (off == 0) {
        s->ihl = ihl;
        k_memcpy(s->buf + FRAG_HDR_MAX - ihl, pkt, ihl);
    }

    if (!s->total || s->got != s->total) return;
    if (!s->ihl || s->ihl + s->total > IP_DATAGRAM_MAX) { slot_free(s); return; }

    // Complete. The header is fragment zero's, rewritten to describe
    // the whole: total length, no flags, and a checksum to match, so a
    // port-unreachable quoting it quotes something that parses.
    uint8_t *d = s->buf + FRAG_HDR_MAX - s->ihl;
    uint32_t whole = s->ihl + s->total;
    d[2] = (uint8_t)(whole >> 8); d[3] = (uint8_t)whole;
    d[6] = 0; d[7] = 0;
    d[10] = 0; d[11] = 0;
    uint16_t sum = net_checksum(d, s->ihl);
    d[10] = (uint8_t)(sum >> 8); d[11] = (uint8_t)sum;

    ipv4_deliver(dev, d, s->ihl, whole);
    slot_free(s);
}
