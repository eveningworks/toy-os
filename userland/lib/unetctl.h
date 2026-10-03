#ifndef ULIB_UNETCTL_H
#define ULIB_UNETCTL_H

#include <stdint.h>
#include "lib/uchan_page.h"
#include "net_abi.h"

// The control protocol between /bin/netctl (and the desktop's network
// flyout) and /bin/netd, over uchan -- the initctl shape (lib/uinitctl.h).
// systemd's `networkctl renew|down|up` asks networkd the same way.
//
// **AN ANSWER IS "ACCEPTED", NOT THE OUTCOME.** A DHCP exchange can take
// 4-12 s and netd serves every card from one loop, so it replies at once
// and acts on its next pass; a caller watches QUERY_NETDEV for the result
// (`netctl` waits on it, the flyout's once-a-second poll shows it).

#define NETCTL_SERVICE "netd"

#define NETCTL_RENEW 1   // ask for the lease again (the remembered address first)
#define NETCTL_DOWN  2   // administratively down, address cleared, no leasing
#define NETCTL_UP    3   // up again, and lease at once

// A reply's `result`.
#define NETCTL_OK       0
#define NETCTL_NO_SUCH  1   // no card by that name
#define NETCTL_REFUSED  2   // known, but not now (renewing a card that is down)

struct netctl_msg {
    uint32_t verb;
    uint32_t result;   // reply only; a request leaves it 0
    char     dev[NET_ABI_NAME_MAX];
};

_Static_assert(sizeof(struct netctl_msg) <= UCHAN_SLOT_BYTES,
               "a netctl message must fit one channel slot");

#endif
