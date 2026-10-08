#ifndef ABI_NET_ABI_H
#define ABI_NET_ABI_H

#include <stdint.h>

// The kernel<->userland networking contract: what a socket call takes,
// and how a device is named and configured.
//
// EVERY IPv4 ADDRESS HERE IS HOST BYTE ORDER, and that is a deliberate
// divergence from POSIX, where sockaddr_in carries a big-endian one and
// every program calls htonl(). The wire order is the STACK's business;
// an app that has to byte-swap to name a host is being asked to know
// something about Ethernet. The conversion happens in one place
// (kernel/net/), which is also the only place a missed swap could hide.

#define NET_ABI_AF_INET      2
#define NET_ABI_SOCK_DGRAM   2
#define NET_ABI_IPPROTO_ICMP 1
#define NET_ABI_IPPROTO_UDP  17
#define NET_ABI_SOCK_STREAM  1
#define NET_ABI_IPPROTO_TCP  6

// Must match NET_NAME_MAX (kernel/include/kernel/netdev.h): an
// interface name is "en" plus where the device is, and its longest
// form is the MAC fallback, "enx54ee75718ebf" -- 15 and a NUL.
#define NET_ABI_NAME_MAX 16

// The longest hostname SYS_NET_RESOLVED carries, NUL included.
// Matches QUERY_CONNLOG_HOST_MAX, which is where such a name ends
// up -- two different caps would truncate silently at whichever is
// smaller.
#define NET_ABI_HOST_MAX 64

// The largest datagram either direction: the largest IPv4 datagram,
// minus the IP and transport headers. Past one MTU it travels in
// fragments. Both transports have an 8-byte header, so one number
// serves both.
#define SYS_NET_MSG_MAX 65507

// The most ONE stream read returns, whatever buffer it is handed -- a
// kernel bounce buffer of this size is allocated per read. Callers must
// still expect any smaller count, as with every stream.
#define SYS_NET_STREAM_READ_MAX 65536

// Ephemeral ports -- what SYS_BIND allocates when asked for port 0.
// IANA's range; Linux uses 32768-60999 and nothing here wants the
// wider one. In the ABI because a caller can SEE the number it was
// given, so it is part of the contract rather than an internal choice.
// How long SYS_CONNECT waits when the caller names no timeout. Six
// retransmits of a SYN at a doubling 200 ms floor is about this.
#define SYS_NET_CONNECT_MS 10000

#define NET_PORT_EPHEMERAL_LO 49152
#define NET_PORT_EPHEMERAL_HI 65535

// SYS_SENDTO and SYS_RECVFROM both take one of these by pointer.
//
// A STRUCT rather than four registers because the syscall table carries
// three arguments, and this kernel's answer to a fourth is a struct --
// SYS_MKPART and SYS_SPAWN made the same call. It also puts the sender's
// address and the buffer in one object, so a receive reports WHO sent
// the datagram without a second out-parameter.
struct net_msg {
    uint64_t buf;   // the payload (the transport header is the kernel's)
    uint32_t len;   // in: bytes to send, or the buffer's capacity
    uint32_t addr;  // in: the destination. out: the sender.
    uint16_t port;  // UDP: in the destination port, out the sender's.
                    // Ignored for ICMP, whose demux key is an
                    // identifier the kernel owns.
    uint16_t pad;   // explicit, so the struct's size is not a
                    // compiler's opinion about alignment

    // SYS_RECVFROM ONLY. How long to wait for a datagram: 0 blocks
    // until one arrives (or a signal interrupts), and any other value
    // is a ceiling in milliseconds after which the call returns 0.
    //
    // ON THE CALL rather than on the socket, because this kernel has no
    // setsockopt and adding one for a single option is worse than the
    // divergence. recvmmsg(2) takes a timeout argument for the same
    // reason. The wait is ALSO interruptible: a signal rewinds the
    // syscall, so Ctrl-C reaches a program parked in a receive.
    uint32_t timeout_ms;
    // SYS_BIND ONLY, and empty means "any device". Binding a socket to
    // one card is Linux's SO_BINDTODEVICE, and it is here for the
    // reason dhclient uses it: a DHCP client must broadcast from
    // 0.0.0.0 out of a NAMED interface, before any interface has an
    // address to route by.
    char dev[NET_ABI_NAME_MAX];
};

// SYS_NET_CONFIG's argument. A zero field is LEFT ALONE rather than
// cleared, so `netctl address net0 10.0.2.20` can change an address without
// restating the netmask -- and clearing one is therefore impossible,
// which is the honest cost of that convenience.
struct net_ifconfig {
    char name[NET_ABI_NAME_MAX];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t flags;   // NET_IFC_*, applied BEFORE the fields above
};
// CLEAR zeroes the address, netmask and gateway (`netctl` releasing a
// card) -- the one way to undo the zero-is-left-alone rule. DOWN and UP
// set the administrative state (Linux's `ip link set ... down`); both
// at once is -EINVAL.
#define NET_IFC_CLEAR 0x01u
#define NET_IFC_DOWN  0x02u
#define NET_IFC_UP    0x04u

// SYS_NET_RENAME. `name` is the interface as it is called now, `to` is
// what it should be called instead.
struct net_rename {
    char name[NET_ABI_NAME_MAX];
    char to[NET_ABI_NAME_MAX];
};

// SYS_NET_LINK's argument: one card's ADAPTER settings -- what it offers
// when it negotiates, and how it behaves -- as Windows' Advanced tab and
// `ethtool -s`/`--set-eee`/`-A`/`-C` set them. `which` names the fields
// to change (NET_LINK_*); the rest are left as they are. A driver
// declares which it can change (query_netdev.link_caps), and a field it
// cannot is -ENOTSUP rather than ignored.
struct net_linkcfg {
    char name[NET_ABI_NAME_MAX];
    uint32_t which;        // NET_LINK_*; NET_LINK_DEFAULTS resets every field
    uint32_t rates;        // NET_RATE_*: the rates it may link at; never 0
    uint32_t eee;          // 0 off, 1 on
    uint32_t flow;         // NET_FLOW_* -- 0 is no pause frames
    uint32_t moderation;   // NET_MOD_*
};
#define NET_LINK_RATES      0x01u
#define NET_LINK_EEE        0x02u
#define NET_LINK_FLOW       0x04u
#define NET_LINK_MODERATION 0x08u
#define NET_LINK_ALL        0x0Fu
#define NET_LINK_DEFAULTS   0x80000000u   // the driver's own values, all of them

#define NET_RATE_10M   0x01u
#define NET_RATE_100M  0x02u
#define NET_RATE_1G    0x04u
#define NET_RATE_2G5   0x08u
#define NET_RATE_5G    0x10u
#define NET_RATE_10G   0x20u
#define NET_RATE_ALL   0x3Fu

#define NET_FLOW_RX 0x1u   // honour pause frames the switch sends
#define NET_FLOW_TX 0x2u   // send them when our buffers fill

// Interrupt moderation, coarse on purpose: how long a card may hold a
// receive interrupt back, each driver mapping it to its own registers.
#define NET_MOD_OFF    0u   // one interrupt per frame: lowest latency
#define NET_MOD_LOW    1u
#define NET_MOD_MEDIUM 2u
#define NET_MOD_HIGH   3u   // fewest interrupts: most throughput per CPU

// SYS_NET_ARP_PROBE's argument: which device to ask on, and the address
// to ask about. `ip` is host byte order like every address above the
// wire here.
struct net_arp_probe {
    char name[NET_ABI_NAME_MAX];
    uint32_t ip;
};


// SYS_NET_RESOLVED's argument: a name a ring-3 resolver has just
// looked up, and what it resolved to.
//
// THE KERNEL NEVER PARSES DNS. It keeps a small (address -> name) cache
// purely so the connection log can print a name beside an address, and
// the cache is fed by whoever did the resolving -- which is Sysmon's
// shape (its Event 22 DNS records are what give its Event 3 connection
// records a name) rather than Zeek's, which snoops the wire.
//
// WHAT THAT COSTS, stated plainly: any process may claim any name for
// any address, so a name here is what a program SAID, not what the
// network answered. There is no privilege model in this kernel to gate
// it with, and the address in a log record is always the real one.
struct net_resolved {
    uint32_t ip;                        // host byte order
    char name[NET_ABI_HOST_MAX];        // NUL-terminated; longer is refused
};

#endif
