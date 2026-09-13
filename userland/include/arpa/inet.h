#ifndef ULIB_ARPA_INET_H
#define ULIB_ARPA_INET_H

// Byte-order conversion. This is the whole of <arpa/inet.h> that exists
// here: there is no inet_addr, inet_ntoa or inet_pton, because nothing
// in this system parses a dotted-quad in ring 3 -- the network stack is
// the kernel's (docs/conventions/kernel.md) and takes addresses as
// integers.
//
// x86-64 is little-endian, so the host-to-network pair is a byte swap
// and the network-to-host pair is the same swap again. __builtin_bswap
// compiles to one BSWAP instruction.

#include <stdint.h>

static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }

#endif
