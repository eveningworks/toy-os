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

#define NET_ABI_NAME_MAX 8   // "net0" -- matches NET_NAME_MAX

// The largest datagram either direction. One IPv4 datagram inside a
// 1500-byte MTU, minus the IP and ICMP headers -- a bigger one would
// need fragmentation, which kernel/net/ipv4.c does not do.
#define SYS_NET_MSG_MAX 1472

// SYS_SENDTO and SYS_RECVFROM both take one of these by pointer.
//
// A STRUCT rather than four registers because the syscall table carries
// three arguments, and this kernel's answer to a fourth is a struct --
// SYS_MKPART and SYS_SPAWN made the same call. It also puts the sender's
// address and the buffer in one object, so a receive reports WHO sent
// the datagram without a second out-parameter.
struct net_msg {
    uint64_t buf;   // the payload (ICMP's header is the kernel's)
    uint32_t len;   // in: bytes to send, or the buffer's capacity
    uint32_t addr;  // in: the destination. out: the sender.
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

#endif
