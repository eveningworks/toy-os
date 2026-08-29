#ifndef KERNEL_NET_H
#define KERNEL_NET_H

#include <stdint.h>
#include "netdev.h"

// The protocol stack: Ethernet, ARP, IPv4, ICMP. It sits ABOVE the
// net_device class and never touches hardware -- one layer per file in
// kernel/net/, the split Linux makes between net/ and drivers/net/.
//
// WHAT THIS DELIBERATELY IS NOT. No fragmentation (a fragmented
// datagram is dropped, not reassembled -- see ipv4.c), no IP options
// on transmit, no routing table beyond "on my subnet, or via the
// gateway", no TCP, no UDP, no IPv6, no multicast beyond broadcast
// ARP. Each of those is a roadmap item, and each would be a file
// beside these rather than a change to them.
//
// ADDRESSES ARE HOST BYTE ORDER EVERYWHERE IN THIS API, and are
// converted at the wire edge by the ntoh/hton helpers below. The trap
// this avoids is the usual one: a uint32_t IP is the same type in both
// orders, so a missed conversion is invisible to the compiler and
// shows up as a packet nobody answers.

// --- byte order -------------------------------------------------------
// x86-64 is little-endian and the wire is big-endian, so these are
// real swaps rather than the no-ops they are on a big-endian host.
static inline uint16_t net_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t net_ntohs(uint16_t v) { return net_htons(v); }
static inline uint32_t net_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t net_ntohl(uint32_t v) { return net_htonl(v); }

#define NET_IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

// --- Ethernet (eth.c) -------------------------------------------------

#define ETH_HDR_LEN     14
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806

extern const uint8_t ETH_BROADCAST[NET_MAC_LEN];

// One received frame, from net_poll(). Not from an interrupt.
void eth_input(struct net_device *dev, const uint8_t *frame, uint32_t len);

// Build a frame around `payload` and transmit it. The payload is
// copied into a static scratch frame, which is what keeps every layer
// above from needing to reserve header room in its own buffer -- safe
// only because nothing here runs from an interrupt or preempts.
int eth_output(struct net_device *dev, const uint8_t dst_mac[NET_MAC_LEN],
               uint16_t ethertype, const void *payload, uint32_t len);

// --- ARP (arp.c) ------------------------------------------------------

void arp_input(struct net_device *dev, const uint8_t *pkt, uint32_t len);

// Look `ip` up in the cache. Returns 1 with `out_mac` filled, or 0
// having SENT a request -- an unresolved address is not an error, it
// is the first half of a resolution the caller retries.
int arp_resolve(struct net_device *dev, uint32_t ip, uint8_t out_mac[NET_MAC_LEN]);

// Cache contents, for `arp` and the KTESTs. `index` walks all entries.
struct arp_entry_view { uint32_t ip; uint8_t mac[NET_MAC_LEN]; const char *dev; };
int arp_cache_count(void);
int arp_cache_at(int index, struct arp_entry_view *out);
void arp_cache_flush(void);

// --- IPv4 (ipv4.c) ----------------------------------------------------

#define IP_PROTO_ICMP 1

void ipv4_input(struct net_device *dev, const uint8_t *pkt, uint32_t len);

// Route, resolve and transmit one datagram. Returns 0, or a negative
// errno -- notably -EAGAIN when the next hop's MAC is not cached yet,
// which is a RETRY rather than a failure (the ARP request is already
// on the wire by then).
int ipv4_output(struct net_device *dev, uint32_t dst_ip, uint8_t proto,
                const void *payload, uint32_t len);

// Which device reaches `dst`, and through which next-hop IP.
struct net_device *ipv4_route(uint32_t dst, uint32_t *out_next_hop);

// The one's-complement sum every header here carries.
uint16_t net_checksum(const void *data, uint32_t len);

// --- ICMP (icmp.c) ----------------------------------------------------

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

void icmp_input(struct net_device *dev, uint32_t src_ip, const uint8_t *pkt, uint32_t len);

// --- sockets (socket.c) ----------------------------------------------
//
// The kernel half of SYS_SOCKET/SYS_SENDTO/SYS_RECVFROM. A socket here
// is an index into a small table, not a pointer -- the fd table stores
// the index, so a stale fd cannot dereference anything.

#define NET_AF_INET     2
#define NET_SOCK_DGRAM  2

int net_sock_open(int domain, int type, int protocol);   // >= 0, or -errno
void net_sock_close(int sock);
int net_sock_sendto(int sock, uint32_t dst_ip, const void *buf, uint32_t len);
// Returns bytes copied, 0 when nothing has arrived (never blocks), or
// -errno. `out_src` is the sender's address.
int net_sock_recvfrom(int sock, void *buf, uint32_t cap, uint32_t *out_src);

// ICMP delivers an echo reply here; returns 1 if a socket wanted it.
int net_sock_deliver(uint8_t proto, uint32_t src_ip, const uint8_t *data, uint32_t len);

#endif
