// ARP: which MAC address answers for an IP, on Ethernet.
//
// The cache is a fixed table with a round-robin victim rather than an
// LRU, because a table this size on a network this size never reaches
// the point where the difference is measurable -- and an eviction
// policy that cannot be observed is one nobody can debug.
//
// AN ENTRY IS KEYED BY (device, IP), not by IP. Two cards on two
// subnets can each hold an entry for 192.168.1.1 and they are
// different machines; a cache keyed on the address alone silently
// sends one subnet's traffic at the other's gateway.
//
// NO RESOLVED ENTRY EXPIRES. Real stacks age entries out (Linux
// revalidates after ~60s) because a machine's MAC can change under its
// IP. Nothing here reaches the age of that mattering, and
// arp_cache_flush() is the escape hatch -- a lever rather than a timer.
//
// AN UNRESOLVED ADDRESS IS ALSO AN ENTRY, and that is what rate-limits
// the requests. Without it every caller retry emits a fresh broadcast:
// `ping` polls every 10 ms for a second, so two pings at an address
// nobody answers put ONE HUNDRED AND FOUR frames on the wire (measured,
// which is how this was found). Linux calls the same state INCOMPLETE
// and retransmits about once a second; the shape here is the same and
// the table is one field wider.
#include "net.h"
#include "netdev.h"
#include "clocksource.h"   // the ARP retransmit interval is real time
#include "string.h"

#define ARP_CACHE_MAX 16
#define ARP_RETRY_NS  (1000ull * 1000ull * 1000ull)   // one request a second

// The packet, for Ethernet/IPv4 only -- the general form carries
// variable-length addresses, and every field below assumes 6 and 4.
struct arp_packet {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sender_mac[NET_MAC_LEN];
    uint32_t sender_ip;       // big-endian, as it sits on the wire
    uint8_t  target_mac[NET_MAC_LEN];
    uint32_t target_ip;
} __attribute__((packed));

_Static_assert(sizeof(struct arp_packet) == 28, "ARP packet is 28 bytes on the wire");

#define ARP_HTYPE_ETHERNET 1
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2

struct arp_entry {
    struct net_device *dev;   // NULL means the slot is free
    uint32_t ip;
    uint8_t mac[NET_MAC_LEN];
    uint8_t resolved;         // 0 = a request is outstanding, no MAC yet
    uint64_t asked_ns;        // when the last request went out
};

static struct arp_entry g_cache[ARP_CACHE_MAX];
static int g_victim;

static struct arp_entry *cache_find(struct net_device *dev, uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_MAX; i++)
        if (g_cache[i].dev == dev && g_cache[i].ip == ip) return &g_cache[i];
    return 0;
}

// A free slot, or the round-robin victim. Never evicts a slot this
// resolution is about to fill.
static struct arp_entry *cache_slot(struct net_device *dev, uint32_t ip) {
    struct arp_entry *e = cache_find(dev, ip);
    if (e) return e;
    for (int i = 0; i < ARP_CACHE_MAX; i++)
        if (!g_cache[i].dev) { e = &g_cache[i]; break; }
    if (!e) {
        e = &g_cache[g_victim];
        g_victim = (g_victim + 1) % ARP_CACHE_MAX;
    }
    k_memset(e, 0, sizeof *e);
    e->dev = dev;
    e->ip = ip;
    return e;
}

static void cache_insert(struct net_device *dev, uint32_t ip, const uint8_t *mac) {
    if (!ip) return;
    struct arp_entry *e = cache_slot(dev, ip);
    k_memcpy(e->mac, mac, NET_MAC_LEN);
    e->resolved = 1;
}

static void send_packet(struct net_device *dev, uint16_t op, uint32_t target_ip,
                        const uint8_t *target_mac, const uint8_t *dst_mac) {
    struct arp_packet p;
    p.htype = net_htons(ARP_HTYPE_ETHERNET);
    p.ptype = net_htons(ETH_TYPE_IPV4);
    p.hlen = NET_MAC_LEN;
    p.plen = 4;
    p.op = net_htons(op);
    k_memcpy(p.sender_mac, dev->mac, NET_MAC_LEN);
    p.sender_ip = net_htonl(dev->ip);
    k_memcpy(p.target_mac, target_mac, NET_MAC_LEN);
    p.target_ip = net_htonl(target_ip);
    eth_output(dev, dst_mac, ETH_TYPE_ARP, &p, sizeof p);
}

void arp_input(struct net_device *dev, const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct arp_packet)) return;
    struct arp_packet p;
    k_memcpy(&p, pkt, sizeof p);   // the payload is not guaranteed aligned

    if (net_ntohs(p.htype) != ARP_HTYPE_ETHERNET) return;
    if (net_ntohs(p.ptype) != ETH_TYPE_IPV4) return;
    if (p.hlen != NET_MAC_LEN || p.plen != 4) return;

    uint32_t sender = net_ntohl(p.sender_ip);
    uint32_t target = net_ntohl(p.target_ip);

    // Learn from anything addressed to us, request or reply alike --
    // a host that asks for our address is one we are about to answer.
    if (sender && target == dev->ip) cache_insert(dev, sender, p.sender_mac);

    if (net_ntohs(p.op) == ARP_OP_REQUEST && dev->ip && target == dev->ip)
        send_packet(dev, ARP_OP_REPLY, sender, p.sender_mac, p.sender_mac);
    else if (net_ntohs(p.op) == ARP_OP_REPLY && sender)
        cache_insert(dev, sender, p.sender_mac);
}

int arp_resolve(struct net_device *dev, uint32_t ip, uint8_t out_mac[NET_MAC_LEN]) {
    if (!dev || !ip) return 0;

    struct arp_entry *e = cache_find(dev, ip);
    if (e && e->resolved) {
        k_memcpy(out_mac, e->mac, NET_MAC_LEN);
        return 1;
    }

    uint64_t now = clocksource_now_ns();
    if (e && now - e->asked_ns < ARP_RETRY_NS) return 0;   // one is already in flight

    e = cache_slot(dev, ip);
    e->asked_ns = now;
    static const uint8_t unknown[NET_MAC_LEN] = { 0, 0, 0, 0, 0, 0 };
    send_packet(dev, ARP_OP_REQUEST, ip, unknown, ETH_BROADCAST);
    return 0;
}

int arp_cache_count(void) {
    int n = 0;
    for (int i = 0; i < ARP_CACHE_MAX; i++) if (g_cache[i].dev && g_cache[i].resolved) n++;
    return n;
}

int arp_cache_at(int index, struct arp_entry_view *out) {
    if (!out || index < 0) return 0;
    int n = 0;
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (!g_cache[i].dev || !g_cache[i].resolved) continue;
        if (n++ != index) continue;
        out->ip = g_cache[i].ip;
        k_memcpy(out->mac, g_cache[i].mac, NET_MAC_LEN);
        out->dev = g_cache[i].dev->name;
        return 1;
    }
    return 0;
}

void arp_cache_flush(void) {
    k_memset(g_cache, 0, sizeof g_cache);
    g_victim = 0;
}

// One device's entries only -- what net_unregister() calls when a card
// is unplugged. Leaving them would be worse than untidy: the driver's
// struct is static and can be registered again, so a stale entry would
// be MATCHED by the same adapter replugged onto a different network,
// and its first frame sent to a MAC that is not there any more.
void arp_flush_device(const struct net_device *dev) {
    if (!dev) return;
    for (int i = 0; i < ARP_CACHE_MAX; i++)
        if (g_cache[i].dev == dev) g_cache[i].dev = 0;
}
