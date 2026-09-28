// The kernel debugger's NETWORK transport -- stage 3 of
// docs/kdebug-design.md, KDNET's shape. A NIC the debugger owns
// (kdebug_nic.h), its own ARP and UDP framing, and every datagram
// authenticated with a key from the boot line.
//
// NOT THE KERNEL'S NETWORK STACK, deliberately: kernel/net/ is frozen
// mid-call whenever the machine is stopped, and has static buffers and
// an ARP cache this could corrupt. Only its pure checksum is borrowed.
// Replies go to the MAC, IP and port of the last authenticated
// datagram, so the stub never has to resolve anything itself.
#include "kdebug_internal.h"
#include "kdebug_nic.h"
#include "ksha256.h"
#include "net.h"      // net_checksum() -- pure
#include "pci.h"
#include "pci_driver.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

static const struct kdb_nic *const g_backends[] = { &kdb_nic_e1000, &kdb_nic_r8169 };

// The PCI "driver" `lspci` shows for the card the debugger took.
static const struct pci_driver g_owner = { .name = "kdebug" };

static const struct kdb_nic *g_nic;
static uint8_t  g_mac[6];
static uint32_t g_ip;
static uint16_t g_port;
static uint8_t  g_key[KDB_NET_KEY_MAX];
static int      g_klen;
static uint64_t g_rx_seq, g_tx_seq;
static uint16_t g_ip_id;

static int      g_have_peer;
static uint8_t  g_peer_mac[6];
static uint32_t g_peer_ip;
static uint16_t g_peer_port;

#define FRAME_MAX 1514
#define PAYLOAD_MAX 1400             // under 1500 - IP - UDP - our header
static uint8_t g_rxf[FRAME_MAX + 4];
static uint8_t g_txf[FRAME_MAX + 4];
static uint8_t g_out[PAYLOAD_MAX];
static int     g_out_len;
static uint8_t g_in[4096];
static uint32_t g_in_head, g_in_tail;

// --- configuration -----------------------------------------------------

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A decimal up to `max`, advancing *p. 0 digits is a failure.
static int dec(const char **p, uint32_t max, uint32_t *out) {
    uint32_t v = 0;
    int n = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (uint32_t)(**p - '0');
        if (v > max) return 0;
        (*p)++, n++;
    }
    *out = v;
    return n > 0;
}

static int hexnum(const char **p, uint32_t max, uint32_t *out) {
    uint32_t v = 0;
    int n = 0, d;
    while ((d = hexval(**p)) >= 0) {
        v = v * 16 + (uint32_t)d;
        if (v > max) return 0;
        (*p)++, n++;
    }
    *out = v;
    return n > 0;
}

static int starts(const char *s, const char *prefix) {
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}

// A PARSER REJECTS RATHER THAN GUESSES: an unknown word, a short key or
// a malformed address refuses the whole line, and the stub stays off.
int kdb_net_parse(const char *v, struct kdb_net_cfg *c) {
    k_memset(c, 0, sizeof *c);
    c->port = 50000;
    c->nic_bus = c->nic_dev = c->nic_fn = -1;
    if (!starts(v, "net")) return 0;
    const char *p = v + 3;
    int have_ip = 0;
    while (*p == ',') {
        p++;
        uint32_t a, b, d, e;
        if (starts(p, "ip=")) {
            p += 3;
            if (!dec(&p, 255, &a) || *p++ != '.' || !dec(&p, 255, &b) || *p++ != '.' ||
                !dec(&p, 255, &d) || *p++ != '.' || !dec(&p, 255, &e))
                return 0;
            c->ip = (a << 24) | (b << 16) | (d << 8) | e;
            have_ip = 1;
        } else if (starts(p, "port=")) {
            p += 5;
            if (!dec(&p, 65535, &a) || a == 0) return 0;
            c->port = (uint16_t)a;
        } else if (starts(p, "key=")) {
            p += 4;
            c->klen = 0;
            while (hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
                if (c->klen == KDB_NET_KEY_MAX) return 0;
                c->key[c->klen++] = (uint8_t)(hexval(p[0]) << 4 | hexval(p[1]));
                p += 2;
            }
        } else if (starts(p, "nic=")) {
            p += 4;
            if (!hexnum(&p, 255, &a) || *p++ != ':' || !hexnum(&p, 31, &b) ||
                *p++ != '.' || !dec(&p, 7, &d))
                return 0;
            c->nic_bus = (int)a, c->nic_dev = (int)b, c->nic_fn = (int)d;
        } else if (starts(p, "wait")) {
            p += 4;
            c->wait = 1;
        } else {
            return 0;
        }
    }
    return *p == 0 && have_ip && c->klen >= KDB_NET_KEY_MIN;
}

void kdb_net_configure(const struct kdb_net_cfg *c) {
    k_memcpy(g_key, c->key, (size_t)c->klen);
    g_klen = c->klen;
    g_ip = c->ip;
    g_port = c->port;
    g_rx_seq = g_tx_seq = 0;
    g_have_peer = 0;
    g_in_head = g_in_tail = 0;
    g_out_len = 0;
}

// --- authentication ----------------------------------------------------

// RFC 2104 over two pieces, so the header and payload need not be copied
// together. The key is at most one block (KDB_NET_KEY_MAX), so it is
// never hashed first.
void kdb_hmac_sha256(const uint8_t *key, int klen, const uint8_t *a, int alen,
                     const uint8_t *b, int blen, uint8_t out[32]) {
    uint8_t pad[64];
    struct ksha256 c;
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < klen ? key[i] : 0) ^ 0x36);
    ksha256_init(&c);
    ksha256_update(&c, pad, 64);
    ksha256_update(&c, a, (size_t)alen);
    ksha256_update(&c, b, (size_t)blen);
    uint8_t inner[32];
    ksha256_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < klen ? key[i] : 0) ^ 0x5c);
    ksha256_init(&c);
    ksha256_update(&c, pad, 64);
    ksha256_update(&c, inner, 32);
    ksha256_final(&c, out);
}

int kdb_net_seal(const char magic[4], uint64_t seq, const uint8_t *payload, int len,
                 uint8_t *out, int cap) {
    if (len < 0 || KDB_NET_HDR + len > cap) return -1;
    k_memcpy(out, magic, 4);
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(seq >> (i * 8));
    uint8_t mac[32];
    kdb_hmac_sha256(g_key, g_klen, out, 12, payload, len, mac);
    k_memcpy(out + 12, mac, 16);
    k_memcpy(out + KDB_NET_HDR, payload, (size_t)len);
    return KDB_NET_HDR + len;
}

// A host datagram's payload length, or -1: wrong magic, a forged or
// damaged MAC, or a sequence number not above the last accepted one.
int kdb_net_open(const uint8_t *d, int len, const uint8_t **payload) {
    if (len < KDB_NET_HDR || d[0] != 'T' || d[1] != 'K' || d[2] != 'D' || d[3] != 'H')
        return -1;
    uint64_t seq = 0;
    for (int i = 0; i < 8; i++) seq |= (uint64_t)d[4 + i] << (i * 8);
    uint8_t mac[32];
    kdb_hmac_sha256(g_key, g_klen, d, 12, d + KDB_NET_HDR, len - KDB_NET_HDR, mac);
    uint8_t diff = 0;   // constant time: no early exit on the first mismatch
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(mac[i] ^ d[12 + i]);
    if (diff || seq <= g_rx_seq) return -1;
    g_rx_seq = seq;
    *payload = d + KDB_NET_HDR;
    return len - KDB_NET_HDR;
}

// --- framing -----------------------------------------------------------

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

// ARP, reply or announcement: we are `g_ip` at `g_mac`.
static void arp_send(uint16_t oper, const uint8_t *to_mac, uint32_t to_ip) {
    uint8_t *f = g_txf;
    k_memcpy(f, to_mac, 6);
    k_memcpy(f + 6, g_mac, 6);
    put16(f + 12, 0x0806);
    put16(f + 14, 1);            // Ethernet
    put16(f + 16, 0x0800);       // IPv4
    f[18] = 6; f[19] = 4;
    put16(f + 20, oper);
    k_memcpy(f + 22, g_mac, 6);
    put32(f + 28, g_ip);
    k_memcpy(f + 32, to_mac, 6);
    put32(f + 38, to_ip);
    g_nic->send(f, 60);          // padded to the Ethernet minimum
}

static void in_push(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t next = (g_in_head + 1) % sizeof g_in;
        if (next == g_in_tail) return;   // full: RSP's checksum and resend cover it
        g_in[g_in_head] = p[i];
        g_in_head = next;
    }
}

static void frame_in(const uint8_t *f, int len) {
    if (len < 42) return;
    uint16_t type = get16(f + 12);
    if (type == 0x0806) {
        if (get16(f + 20) == 1 && get32(f + 38) == g_ip)
            arp_send(2, f + 22, get32(f + 28));
        return;
    }
    if (type != 0x0800) return;
    const uint8_t *ip = f + 14;
    int ihl = (ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 || get32(ip + 16) != g_ip) return;
    if (get16(ip + 6) & 0x3FFF) return;   // a fragment: never ours
    int total = get16(ip + 2);
    if (total > len - 14 || total < ihl + 8) return;
    const uint8_t *udp = ip + ihl;
    if (get16(udp + 2) != g_port) return;
    int ulen = get16(udp + 4);
    if (ulen < 8 || ulen > total - ihl) return;

    const uint8_t *payload;
    int n = kdb_net_open(udp + 8, ulen - 8, &payload);
    if (n < 0) return;   // unauthenticated, damaged or replayed: silence
    k_memcpy(g_peer_mac, f + 6, 6);
    g_peer_ip = get32(ip + 12);
    g_peer_port = get16(udp);
    g_have_peer = 1;
    in_push(payload, n);
}

// ONLY WHILE A WHOLE PAYLOAD FITS: a frame taken off the card is kept or
// lost, so taking one the FIFO cannot hold drops the middle of a packet.
// Small protocol packets never came near the limit; `remote put`'s 4 KiB
// pwrites, several datagrams each, were cut up. STILL BOUNDED: frames
// frame_in() drops never fill the FIFO, and this runs from the tick with
// interrupts off, so a flood of junk must not keep it here.
static void pump(void) {
    for (int i = 0; i < 16; i++) {
        uint32_t used = (g_in_head + sizeof g_in - g_in_tail) % sizeof g_in;
        if (sizeof g_in - 1 - used < PAYLOAD_MAX) return;
        int n = g_nic->recv(g_rxf, FRAME_MAX);
        if (n <= 0) return;
        frame_in(g_rxf, n);
    }
}

// --- the transport -----------------------------------------------------

static int net_getc(void) {
    if (g_in_tail == g_in_head) pump();
    if (g_in_tail == g_in_head) return -1;
    int c = g_in[g_in_tail];
    g_in_tail = (g_in_tail + 1) % sizeof g_in;
    return c;
}

static void net_flush(void) {
    if (!g_out_len) return;
    int len = g_out_len;
    g_out_len = 0;
    if (!g_have_peer) return;   // nobody has spoken yet: nobody to answer

    uint8_t *f = g_txf, *ip = f + 14, *udp = ip + 20;
    int dlen = kdb_net_seal("TKDT", ++g_tx_seq, g_out, len, udp + 8, FRAME_MAX - 42);
    if (dlen < 0) return;
    k_memcpy(f, g_peer_mac, 6);
    k_memcpy(f + 6, g_mac, 6);
    put16(f + 12, 0x0800);
    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, (uint16_t)(20 + 8 + dlen));
    put16(ip + 4, g_ip_id++);
    put16(ip + 6, 0x4000);       // don't fragment
    ip[8] = 64; ip[9] = 17;
    put16(ip + 10, 0);
    put32(ip + 12, g_ip);
    put32(ip + 16, g_peer_ip);
    put16(ip + 10, net_checksum(ip, 20));   // host order, as icmp.c stores it
    put16(udp, g_port);
    put16(udp + 2, g_peer_port);
    put16(udp + 4, (uint16_t)(8 + dlen));
    put16(udp + 6, 0);           // IPv4 UDP checksum is optional; the HMAC is stronger
    int flen = 14 + 20 + 8 + dlen;
    g_nic->send(f, flen < 60 ? 60 : flen);
}

static void net_putc(char c) {
    g_out[g_out_len++] = (uint8_t)c;
    if (g_out_len == PAYLOAD_MAX) net_flush();
}

static const struct kdb_transport kdb_net = { net_getc, net_putc, net_flush };

static int nic_wanted(const struct kdb_net_cfg *c, const struct pci_device *d) {
    return c->nic_bus < 0 || (d->bus == c->nic_bus && d->device == c->nic_dev &&
                              d->function == c->nic_fn);
}

const struct kdb_transport *kdb_net_init(const char *v, int *wait) {
    static struct kdb_net_cfg cfg;   // holds a key: not on a stack that outlives it
    if (!kdb_net_parse(v, &cfg)) {
        klog_write(KLOG_WARN "kdebug: `kdebug=net,...` refused -- it needs ip=A.B.C.D and "
                   "key= of at least 16 bytes in hex; see docs/boot-flags.md\n");
        return 0;
    }
    // The LAST card a backend matches, unless nic= names one: the first
    // is the one the OS's own driver expects to get.
    int pick = -1;
    const struct kdb_nic *nic = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        for (unsigned b = 0; b < sizeof g_backends / sizeof g_backends[0]; b++) {
            if (g_backends[b]->match(d) && nic_wanted(&cfg, d)) {
                pick = i;
                nic = g_backends[b];
            }
        }
    }
    if (pick < 0 || !pci_device_claim(pick, &g_owner) ||
        !nic->claim(pci_device_at(pick), g_mac)) {
        klog_write(KLOG_WARN "kdebug: no NIC the debugger can own -- NOT armed\n");
        k_memset(&cfg, 0, sizeof cfg);
        return 0;
    }
    g_nic = nic;
    kdb_net_configure(&cfg);
    *wait = cfg.wait;
    const struct pci_device *d = pci_device_at(pick);
    klog_printf(KLOG_WARN "kdebug: GDB stub armed on the network -- %s at %02x:%02x.%u, "
                "%u.%u.%u.%u udp %u, %02x:%02x:%02x:%02x:%02x:%02x\n",
                nic->name, d->bus, d->device, d->function,
                g_ip >> 24, (g_ip >> 16) & 255, (g_ip >> 8) & 255, g_ip & 255, g_port,
                g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    k_memset(&cfg, 0, sizeof cfg);
    // Announce the address, so a switch or SLIRP learns where it lives
    // before the first datagram arrives for it.
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    arp_send(2, bcast, g_ip);
    return &kdb_net;
}
