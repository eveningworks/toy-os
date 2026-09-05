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

#define NET_ABI_NAME_MAX 8   // "net0" -- matches NET_NAME_MAX

// The longest hostname SYS_NET_RESOLVED carries, NUL included.
// Matches QUERY_CONNLOG_HOST_MAX, which is where such a name ends
// up -- two different caps would truncate silently at whichever is
// smaller.
#define NET_ABI_HOST_MAX 64

// The largest datagram either direction. One IPv4 datagram inside a
// 1500-byte MTU, minus the IP and transport headers -- a bigger one
// would need fragmentation, which kernel/net/ipv4.c does not do. Both
// transports have an 8-byte header, so one number serves both.
#define SYS_NET_MSG_MAX 1472

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
// cleared, so `ifconfig net0 10.0.2.20` can change an address without
// restating the netmask -- and clearing one is therefore impossible,
// which is the honest cost of that convenience.
struct net_ifconfig {
    char name[NET_ABI_NAME_MAX];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
};

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
