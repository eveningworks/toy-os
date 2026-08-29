#ifndef KERNEL_NET_H
#define KERNEL_NET_H

#include <stdint.h>
#include "netdev.h"
#include "net_abi.h"   // the ephemeral range, and the syscall structs

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
#define IP_PROTO_UDP  17

// The IPv4 broadcast address, which routing and ARP both special-case:
// it is delivered to the Ethernet broadcast address with no resolution,
// and it is the only destination a device with NO address of its own
// may send to -- which is exactly the state a DHCP client starts in.
#define IP_BROADCAST 0xFFFFFFFFu

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

// The same sum over two buffers, as if they were concatenated. UDP
// needs it: its checksum covers a 12-byte PSEUDO-HEADER that appears
// nowhere in the packet, and copying the datagram just to prepend it
// would be a memcpy per datagram to avoid one function.
uint16_t net_checksum_two(const void *a, uint32_t a_len,
                          const void *b, uint32_t b_len);

// --- ICMP (icmp.c) ----------------------------------------------------

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

void icmp_input(struct net_device *dev, uint32_t src_ip, const uint8_t *pkt, uint32_t len);

// "Nothing is listening on that port" -- type 3 code 3. Takes the WHOLE
// offending IPv4 datagram, because what it must quote back is that
// header plus the first 8 bytes after it, and only a caller holding the
// header knows how long it was.
void icmp_send_port_unreachable(struct net_device *dev, uint32_t src_ip,
                                const uint8_t *ip_datagram, uint32_t ip_len);

// --- UDP (udp.c) ------------------------------------------------------

// One datagram's payload, the IP and UDP headers taken off the MTU.
#define NET_UDP_MAX 1472

// Returns 1 when a socket took the datagram and 0 when no port
// matched. The CALLER answers the miss, because the port-unreachable
// report has to quote the IPv4 header and this layer never saw it.
int udp_input(struct net_device *dev, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t *pkt, uint32_t len);

// `dev` NULL routes by destination; naming one sends out that card
// regardless, which is what a broadcast from an unaddressed device
// needs. Returns 0, or a negative errno (-EAGAIN while ARP resolves).
int udp_output(struct net_device *dev, uint32_t dst_ip, uint16_t dst_port,
               uint16_t src_port, const void *payload, uint32_t len);

// --- sockets (socket.c) ----------------------------------------------
//
// The kernel half of SYS_SOCKET/SYS_SENDTO/SYS_RECVFROM. A socket here
// is an index into a small table, not a pointer -- the fd table stores
// the index, so a stale fd cannot dereference anything.

#define NET_AF_INET     2
#define NET_SOCK_DGRAM  2

// Ephemeral ports live in abi/net_abi.h: a caller sees the number
// SYS_BIND hands back, so the range is part of the contract.

int net_sock_open(int domain, int type, int protocol);   // >= 0, or -errno
void net_sock_close(int sock);

// Give the socket a local port, a local address and optionally ONE
// device (Linux's SO_BINDTODEVICE, which is what a DHCP client needs:
// it must broadcast out of a named card before any card has an
// address). A port of 0 asks the kernel to pick an ephemeral one; a
// NULL or empty `dev` means any. -EADDRINUSE if the port is taken.
int net_sock_bind(int sock, uint32_t addr, uint16_t port, const char *dev);

int net_sock_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
                    const void *buf, uint32_t len);
// Returns bytes copied, 0 when nothing is queued, or -errno.
// `out_src`/`out_port` are the sender's. THIS CALL NEVER BLOCKS -- the
// waiting is the syscall layer's, because only it holds the trapframe a
// block needs. See sys_recvfrom().
int net_sock_recvfrom(int sock, void *buf, uint32_t cap,
                      uint32_t *out_src, uint16_t *out_port);

// The absolute deadline a blocked receive on this socket is working to,
// in clocksource_now_ns() terms; 0 when none is set. It lives on the
// SOCKET rather than in the handler because a blocking syscall is
// RE-RUN when it wakes (SYS_RETRY, and a signal rewinds it too) -- a
// deadline held in a local would restart on every wake, so a receive
// interrupted repeatedly would never time out.
uint64_t net_sock_deadline(int sock);
void net_sock_set_deadline(int sock, uint64_t ns);

// ICMP delivers an echo reply here; returns 1 if a socket wanted it.
int net_sock_deliver(uint8_t proto, uint32_t src_ip, const uint8_t *data, uint32_t len);

// UDP delivers here. Returns 1 if a socket took it, 0 if no port
// matched -- which is what makes the port-unreachable reply possible.
int net_sock_deliver_udp(struct net_device *dev, uint32_t src_ip, uint16_t src_port,
                         uint16_t dst_port, const uint8_t *data, uint32_t len);

#endif
