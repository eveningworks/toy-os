#ifndef UDHCP_H
#define UDHCP_H

#include <stdint.h>
#include "net_abi.h"
#include "query_abi.h"

// THE DHCP CLIENT, AS A LIBRARY -- one implementation, two front ends.
//
// It was all of /bin/dhcp, whose supervision loop held ONE lease in
// file-scope state and blocked in sleep() between renewals. That is the
// right shape for a command run by hand and the wrong one for a daemon:
// /bin/netd holds a lease per card, and a machine with two cards had
// only its first one renewed -- the second's address silently expired
// at whatever hour the server chose.
//
// So the state is a STRUCT the caller owns, and the loop is inverted: a
// front end calls udhcp_step() and is told when to come back, rather
// than the library sleeping on its behalf. One card blocks nobody.
//
// It still BLOCKS for the length of one exchange (a few seconds waiting
// for an OFFER or an ACK). Making that asynchronous too would need a
// non-blocking socket and a much larger state machine, and buys little:
// the wait a daemon actually noticed was the ten-second carrier wait,
// and udhcp_step() does not do that at all -- see udhcp_carrier_wait().

struct udhcp_lease {
    uint32_t ip, mask, router, dns, server;
    uint32_t seconds;     // the whole lease
    uint32_t t1, t2;      // renew at, rebind at -- seconds from the ACK
};

enum udhcp_state {
    UDHCP_INIT = 0,   // no lease: keep asking, with backoff
    UDHCP_BOUND,      // holding one: renew at T1, rebind at T2
};

struct udhcp {
    char dev[NET_ABI_NAME_MAX];
    uint8_t mac[6];       // the ABI carries a MAC as a uint64_t, so there
                          // is no constant to share; six is six
    struct udhcp_lease lease;
    uint64_t acked_ns;    // when the held lease was granted. EVERY
                          // deadline is measured from this and never
                          // from the last attempt: a retry that also
                          // pushed expiry back would never expire.
    uint64_t due_ns;      // when udhcp_step() wants to be called again
    uint32_t backoff_ms;  // INIT retry, doubling
    int state;
};

// Where diagnostics go. A service's fd 1 reaches nobody, so a resident
// caller sends them to fd 2, which is the kernel log; a command run at
// a prompt wants fd 1, where the person can see them.
void udhcp_log_to_kernel(int yes);

// Start tracking `dev`. Reads back a remembered lease if there is one,
// so the first step can ask for the address this card had (INIT-REBOOT)
// rather than take whatever is free.
void udhcp_init(struct udhcp *u, const char *dev, const uint8_t *mac);

// Do whatever this interface is due for and return when to come back.
// Calling it early is harmless -- it returns the same deadline again.
uint64_t udhcp_step(struct udhcp *u, uint64_t now_ns);

// When it next wants to run. A caller with several of these sleeps
// until the earliest.
uint64_t udhcp_due(const struct udhcp *u);

// One blocking attempt, for a caller that wants an answer rather than a
// schedule -- `dhcp <card>` typed at a prompt. Waits for carrier first,
// falls back to a link-local address when nothing answers. Returns 1 if
// the card ends up with an address.
int udhcp_once(struct udhcp *u, const struct query_netdev *dev);

// Wait up to ten seconds for the wire, which is what a command run by
// hand should do. A DAEMON MUST NOT: it would stall every other card
// behind a port with no cable in it, so netd polls `link_up` instead
// and simply does not step a card that has no carrier. A driver that
// cannot report carrier is not "down" and returns 1 at once.
int udhcp_carrier_wait(const char *name);

// Give the address up: forget the remembered lease and reset to INIT.
void udhcp_release(struct udhcp *u);

#endif
