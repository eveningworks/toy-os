#ifndef ULIB_UNETLINK_H
#define ULIB_UNETLINK_H

// unetlink -- a network card's ADAPTER settings (the rates it offers,
// Energy Efficient Ethernet, pause frames, interrupt moderation): the
// words /etc/net.conf spells them in, the labels a person reads, and
// applying and saving them. /bin/netd applies the file when a card
// appears; `netctl link`, Device Manager and System Settings change them
// (ui/uui_netadapter.h). `ethtool`'s job, kept where systemd-networkd's
// .link files keep it: in the card's own section of the naming file.
//
//     [a0:36:bc:d7:cb:84]
//     speed = 2.5g          auto, or the fastest rate to offer
//     eee = off             on | off
//     flow = off            off | rx | tx | both
//     moderation = medium   off | low | medium | high

#include <stdint.h>
#include "net_abi.h"
#include "query_abi.h"
#include "etc_config.h"

#define UNETLINK_CONF "/etc/net.conf"

// The sections that can address a card, MOST SPECIFIC FIRST -- its MAC,
// where it is plugged in, its driver -- or NULL where the card cannot
// answer that one (a driver reporting no location), which is a skip, not
// the end. `slot` holds the MAC string and must outlive the use. netd's
// naming rules read the file in this same order.
#define UNETLINK_CARD_KEYS 3
const char *unetlink_card_key(const struct query_netdev *d, int i, char slot[18]);

// --- words and labels --------------------------------------------------

// A SPEED IS A CAP: "1g" offers every rate the card has at or below 1 Gb/s,
// "auto" (0 here) all of them. Parse gives the NET_RATE_* bit, 0 for
// auto, or -1 for a word it does not know.
int64_t unetlink_speed_parse(const char *word);
const char *unetlink_speed_word(uint32_t cap);           // 0 -> "auto"
uint32_t unetlink_rates_for(uint32_t supported, uint32_t cap);
// The inverse, for showing what a card offers now: 0 when it offers all
// it has, else its fastest.
uint32_t unetlink_cap_of(uint32_t supported, uint32_t rates);
const char *unetlink_rate_label(uint32_t rate);         // "2.5 Gb/s", "100 Mb/s"

int unetlink_flow_parse(const char *word);              // NET_FLOW_* or -1
const char *unetlink_flow_word(uint32_t flow);
const char *unetlink_flow_label(uint32_t flow);         // "Receive only"
int unetlink_mod_parse(const char *word);               // NET_MOD_* or -1
const char *unetlink_mod_word(uint32_t mod);
const char *unetlink_mod_label(uint32_t mod);           // "Medium"

// --- the file and the card ---------------------------------------------

// What `conf` asks of card `d`: each field the card can change and some
// section names, in out->which, the most specific section winning per
// key; speed resolved against what the card has. `out->name` is the
// card's. Returns how many fields were named; a word it cannot read is
// skipped and counted in *bad (may be NULL).
int unetlink_load(const struct etc_config_buf *conf, const struct query_netdev *d,
                  struct net_linkcfg *out, int *bad);

// Apply `want`'s fields (want->which) to the card at once, then SAVE them
// to its own [MAC] section, so netd applies them again at the next boot.
// 0, or -1 with sys_errno() -- an apply the kernel refused saves nothing.
int unetlink_set(const struct query_netdev *d, const struct net_linkcfg *want);

// The driver's own values back, and the card's keys out of its section.
int unetlink_reset(const struct query_netdev *d);

#endif // ULIB_UNETLINK_H
