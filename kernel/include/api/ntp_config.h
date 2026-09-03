#ifndef NTP_CONFIG_H
#define NTP_CONFIG_H

#include <stdint.h>

// Registers `system.ntp`, `system.ntp_server` and `system.ntp_interval`.
//
// **THE KERNEL SETTLES NO TIME AND SPEAKS NO NTP.** These are three
// persisted values and nothing else; `/bin/ntpd` reads them, talks to a
// server and applies the answer through SYS_SETTIME. The kernel owns
// the clock, exactly as it owns the network device -- which protocol to
// trust and how often to ask are policy, and the same argument that
// keeps DHCP in ring 3 keeps SNTP there.
//
// They live in the registry rather than in a private /etc file of their
// own so that System Settings shows them without knowing what NTP is,
// and so `config set system.ntp off` is the same gesture as every other
// switch on the machine.
void ntp_setting_register(void);

// The three values, for a kernel-side caller. `/bin/ntpd` reads them
// over the settings ABI instead; these exist because the defaults have
// to be spelled in exactly one place and this is it.
#define NTP_DEFAULT_SERVER   "pool.ntp.org"
#define NTP_DEFAULT_INTERVAL 60   // minutes
#define NTP_INTERVAL_MIN     1
#define NTP_INTERVAL_MAX     1440 // a day; past that a client is not syncing

#endif
