#ifndef ULIB_UINITCTL_H
#define ULIB_UINITCTL_H

#include <stdint.h>
#include "lib/uchan_page.h"

// The control protocol between init and its clients -- `/bin/service`,
// and everything that restarts or powers off the machine -- over uchan.
//
// WHAT IT REPLACES, and why the old shape existed: a request was a LINE
// APPENDED TO /run/init.ctl and the notification was SIGHUP, because
// "there are no unix sockets here, and no named pipes" -- runit's
// control object plus SysV's `kill -HUP 1`. Neither half could be the
// other: a signal carries no payload, and a file could not be noticed
// because init BLOCKS waiting for children. A channel is both halves at
// once, and it can answer.
//
// **THE FILE AND THE DOORBELL STILL WORK.** init serves both, and
// `/bin/service` falls back to them when no beacon is published -- an
// init too old to answer, or one that failed to open its channel. A
// fallback nothing can reach is a guess (`ata nodma`, `nopat`, TFS3 v1),
// so this one stays producible: stop init's channel and the old path
// carries the same commands.

#define INITCTL_SERVICE "initctl"

#define INITCTL_START    1
#define INITCTL_STOP     2
// The machine, not a service: init stops every service in the reverse of
// its start order, then everything else, then calls SYS_POWEROFF. `name`
// is unused. The reply comes BEFORE any of that starts.
#define INITCTL_REBOOT   3
#define INITCTL_POWEROFF 4

// A reply's `result`.
#define INITCTL_OK        0
#define INITCTL_NO_SUCH   1  // no service by that name
#define INITCTL_REFUSED   2  // known, but the verb could not be applied

struct initctl_msg {
    uint32_t verb;
    uint32_t result;   // reply only; a request leaves it 0
    char     name[40];
};

_Static_assert(sizeof(struct initctl_msg) <= UCHAN_SLOT_BYTES,
               "an initctl message must fit one channel slot");

// RESTART (reboot = 1) OR POWER OFF, the way every caller should: asks
// init, which stops the services first. Returns 0 once init has taken it
// -- the machine goes down shortly, and the caller is among what init
// stops. When init cannot be asked (no channel, no answer) it calls
// SYS_POWEROFF itself, which does not return on success: -1 only if
// that failed too. `reboot --force` is the one caller that skips init
// on purpose -- systemd's `reboot -f`.
int uinitctl_shutdown(int reboot);

#endif
