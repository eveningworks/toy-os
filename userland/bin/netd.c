// netd -- the network daemon: what a card is called, and which cards
// get an address.
//
// WHY THIS EXISTS RATHER THAN A BIGGER /bin/dhcp. Two jobs on this
// machine had no owner. Naming was the kernel's, which made a name a
// kernel policy nobody could change without a rebuild. And `dhcp -k`
// supervised exactly ONE card, so the second card on a two-NIC machine
// held a lease nothing renewed -- its address expiring at whatever hour
// the server chose. Neither is a bug in dhcp; both are the shape of a
// program that is one card's client rather than the machine's.
//
// SO IT IS udev PLUS networkd, cut down. The kernel gives a card a
// bootstrap name from its MAC and validates a rename; the rules live in
// /etc/net.conf and are read HERE, in ring 3, which is the same split
// this project already made for NTP, DHCP and DNS. Linux does exactly
// this and for the same reason -- naming policy in a kernel is policy
// nobody can change.
//
// ONE PROCESS, N CARDS, NO THREADS. udhcp_step() advances one interface
// and returns when it wants to be called again, so the loop sleeps until
// the earliest deadline across every card. That is what a per-card
// /bin/dhcp could not do: a process per card would each need its own
// supervision, and a card with no cable would still be waiting out a
// ten-second carrier wait somebody else was queued behind.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "query_abi.h"
#include "lib/cmd.h"
#include "lib/uconf.h"
#include "lib/udhcp.h"
#include "lib/uchan.h"
#include "lib/unetctl.h"
#include <stdio.h>
#include <string.h>

#define NET_CONF "/etc/net.conf"
#define MAX_CARDS 4          // NET_MAX_DEVS; more would be a kernel change

// The whole of this daemon's per-card state. `known` is what stops a
// card being renamed or re-leased every time round the loop.
struct card {
    char name[NET_ABI_NAME_MAX];   // as the kernel calls it NOW
    uint64_t mac;
    int known;
    // ASKED FOR (`netctl renew`/`up`): lease even though the card holds
    // an address and no lease, which the loop otherwise reads as an
    // address set by hand. Cleared once a lease is bound.
    int forced;
    struct udhcp dhcp;
};

static struct card g_cards[MAX_CARDS];
static int g_count;

static void mac_str(char *out, uint64_t mac) {
    unsigned long long m = (unsigned long long)mac;
    snprintf(out, 18, "%02llx:%02llx:%02llx:%02llx:%02llx:%02llx",
             m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF,
             (m >> 24) & 0xFF, (m >> 32) & 0xFF, (m >> 40) & 0xFF);
}

// --- the rules ---------------------------------------------------------
//
// MOST SPECIFIC WINS, and the order is the whole rule: this card, then
// this socket, then this driver. A MAC names one adapter wherever it is
// plugged; a location names whatever is in a slot; a driver names a
// class of card. Anything not matched falls through to the scheme.
//
// The file is re-read on every pass rather than cached. It is under 2 KB
// and the pass is seconds apart, and the alternative is a daemon that
// has to be restarted to pick up an edit -- which is the thing a rules
// file exists to avoid.
// The `i`-th way this card can be addressed, most specific first. NULL
// where the card cannot answer that one (a driver reporting no
// location), which is a SKIP rather than the end of the list.
// `slot` holds the MAC string, which has to outlive the call.
static const char *card_key(const struct query_netdev *d, int i,
                            char *slot, uint32_t cap) {
    (void)cap;
    if (i == 0) { mac_str(slot, d->mac); return slot; }
    if (i == 1) return d->location[0] ? d->location : 0;
    if (i == 2) return d->driver;
    return 0;
}
#define CARD_KEYS 3

static int rule_name(const struct etc_config_buf *buf,
                     const struct query_netdev *d, char *out, uint32_t cap) {
    char key[24];
    for (int i = 0; i < CARD_KEYS; i++) {
        const char *k = card_key(d, i, key, sizeof key);
        if (!k || !*k) continue;
        // A `[<key>]` section with a `name`, then the FLAT `<key> =
        // <name>` line the file used before it had sections. Both are
        // read: an /etc written by an earlier build is still somebody's
        // machine, and a rule that silently stopped applying would show
        // up as a card that renamed itself back.
        if (etc_config_buf_get_in(buf, k, "name", out, cap) && out[0]) return 1;
        if (etc_config_buf_get(buf, k, out, cap) && out[0]) return 1;
    }

    // No explicit name: build one from the scheme. `kernel` is the
    // opt-out and is also what an unrecognised scheme falls back to --
    // a name nobody asked for is worse than the one already there.
    char scheme[16], prefix[16];
    if (!etc_config_buf_get(buf, "scheme", scheme, sizeof scheme) || !scheme[0])
        strcpy(scheme, "mac");
    if (!etc_config_buf_get(buf, "prefix", prefix, sizeof prefix) || !prefix[0])
        strcpy(prefix, "net");

    if (!strcmp(scheme, "mac")) {
        unsigned long long m = (unsigned long long)d->mac;
        snprintf(out, cap, "%s-%02llx%02llx%02llx", prefix,
                 (m >> 24) & 0xFF, (m >> 32) & 0xFF, (m >> 40) & 0xFF);
        return 1;
    }
    if (!strcmp(scheme, "location")) {
        // A card whose driver reports no location keeps what it has,
        // rather than being given a name that says nothing.
        if (!d->location[0]) return 0;
        snprintf(out, cap, "%s-%s", prefix, d->location);
        return 1;
    }
    if (!strcmp(scheme, "driver")) {
        snprintf(out, cap, "%s-%s", prefix, d->driver);
        return 1;
    }
    return 0;   // "kernel", or a scheme this does not know
}

// --- the loop ----------------------------------------------------------

static struct card *card_for(uint64_t mac) {
    // MATCHED ON THE MAC, not the name: a card this daemon renamed is
    // the same card under a name that has changed since the last pass,
    // and matching on the name would make it a new one every time.
    for (int i = 0; i < g_count; i++)
        if (g_cards[i].known && g_cards[i].mac == mac) return &g_cards[i];
    if (g_count >= MAX_CARDS) return 0;
    return &g_cards[g_count++];
}

// Does this card get a lease? Its own `dhcp = yes|no` if it declares
// one, else the file's global `dhcp = all|none`.
//
// A per-card answer needs a per-card SECTION: before sections the card
// WAS the key, so a card could carry exactly one fact -- its name.
// --- the control channel (lib/unetctl.h) --------------------------------

static struct uchan_server g_chan;
static int g_chan_open;

static struct card *card_named(const char *name) {
    for (int i = 0; i < g_count; i++)
        if (g_cards[i].known && !strcmp(g_cards[i].name, name)) return &g_cards[i];
    return 0;
}

static void restart_lease(struct card *c) {
    uint8_t mac[6];
    for (int b = 0; b < 6; b++) mac[b] = (uint8_t)(c->mac >> (b * 8));
    udhcp_init(&c->dhcp, c->name, mac);   // INIT-REBOOT for the remembered address first
    c->forced = 1;
}

// Every verb is a syscall or two, so it is applied HERE and answered at
// once; the lease it starts is the next pass's (lib/unetctl.h).
static int control(const struct netctl_msg *m) {
    struct card *c = card_named(m->dev);
    if (!c) return NETCTL_NO_SUCH;
    struct query_netdev d;
    int down = 0;
    QUERY_FOREACH(QUERY_NETDEV, d, i)
        if (!strcmp(d.name, c->name)) down = (int)d.admin_down;
    switch (m->verb) {
    case NETCTL_RENEW:
        if (down) return NETCTL_REFUSED;
        printf("netd: %s: renewing, as asked\n", c->name);
        restart_lease(c);
        return NETCTL_OK;
    case NETCTL_DOWN:
        // The address goes with it, so nothing routes to a card that
        // will not answer; the lease FILE stays, so `up` asks for the
        // same address again.
        if (sys_net_admin(c->name, NET_IFC_DOWN | NET_IFC_CLEAR) < 0) return NETCTL_REFUSED;
        printf("netd: %s: down, as asked\n", c->name);
        restart_lease(c);
        c->forced = 0;
        return NETCTL_OK;
    case NETCTL_UP:
        if (sys_net_admin(c->name, NET_IFC_UP) < 0) return NETCTL_REFUSED;
        printf("netd: %s: up, as asked\n", c->name);
        restart_lease(c);
        return NETCTL_OK;
    }
    return NETCTL_REFUSED;
}

static void serve_channel(void) {
    if (!g_chan_open) return;
    uchan_server_scan(&g_chan, 0, 0);
    int from;
    struct netctl_msg m;
    while ((from = uchan_server_recv(&g_chan, &m, sizeof m)) != 0) {
        m.dev[sizeof m.dev - 1] = '\0';
        struct netctl_msg reply;
        memset(&reply, 0, sizeof reply);
        reply.verb = m.verb;
        reply.result = (uint32_t)control(&m);
        memcpy(reply.dev, m.dev, sizeof reply.dev);
        uchan_server_reply(&g_chan, from, &reply, sizeof reply);
    }
}

static int wants_dhcp(const struct etc_config_buf *buf,
                      const struct query_netdev *d, const char *global) {
    char key[24], v[8];
    for (int i = 0; i < CARD_KEYS; i++) {
        const char *k = card_key(d, i, key, sizeof key);
        if (!k || !*k) continue;
        if (!etc_config_buf_get_in(buf, k, "dhcp", v, sizeof v) || !v[0]) continue;
        return strcmp(v, "no") != 0 && strcmp(v, "none") != 0 && strcmp(v, "0") != 0;
    }
    return strcmp(global, "all") == 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    udhcp_log_to_kernel(1);   // a service's fd 1 reaches nobody

    // STATIC, NOT A LOCAL. etc_config_buf is 4 KiB and a ring-3 frame is
    // budgeted at 2 KiB against a single guard page, so this on the
    // stack does not merely overflow -- it steps over the guard into
    // unmapped space. -Wframe-larger-than catches it; the fix is that
    // this daemon has one thread and one loop.
    static struct etc_config_buf conf;

    // THE CHANNEL, if it can be had. Without it netd still leases every
    // card; only `netctl renew|down|up` has nobody to ask.
    if (uchan_server_open(&g_chan, NETCTL_SERVICE) == 0) g_chan_open = 1;
    else printf("netd: no control channel -- netctl cannot reach this daemon\n");

    for (;;) {
        serve_channel();
        int have_conf = uconf_load(NET_CONF, &conf);

        char want_dhcp[8] = "all";
        if (have_conf) etc_config_buf_get(&conf, "dhcp", want_dhcp, sizeof want_dhcp);
        if (!want_dhcp[0]) strcpy(want_dhcp, "all");

        uint64_t next = sys_monotonic_ns() + 10ull * 1000000000ull;

        struct query_netdev d;
        for (unsigned i = 0; ; i++) {
            if (sys_query_record(QUERY_NETDEV, i, &d, sizeof d) < (int)sizeof d) break;

            struct card *c = card_for(d.mac);
            if (!c) continue;

            if (!c->known) {
                c->mac = d.mac;
                c->known = 1;
                strncpy(c->name, d.name, sizeof c->name - 1);

                char chosen[NET_ABI_NAME_MAX];
                if (have_conf && rule_name(&conf, &d, chosen, sizeof chosen) &&
                    strcmp(chosen, d.name) != 0) {
                    if (sys_net_rename(d.name, chosen) == 0) {
                        printf("netd: %s is now %s\n", d.name, chosen);
                        strncpy(c->name, chosen, sizeof c->name - 1);
                    } else {
                        // Refused: taken, or a shape a lease filename
                        // could not survive. Said out loud, because a
                        // rule that silently does nothing is worse than
                        // one that does not exist.
                        printf("netd: cannot name %s as %s\n", d.name, chosen);
                    }
                }

                uint8_t mac[6];
                for (int b = 0; b < 6; b++) mac[b] = (uint8_t)(d.mac >> (b * 8));
                udhcp_init(&c->dhcp, c->name, mac);
            }

            if (!wants_dhcp(&conf, &d, want_dhcp)) continue;
            if (d.admin_down) continue;   // `netctl down`: switched off on purpose

            // A card holding a lease whose kernel-side address is gone
            // is a card that LEFT AND CAME BACK -- its driver was
            // unloaded and reloaded -- and the lease is for a device
            // that no longer exists. Start it over.
            if (!d.ip && c->dhcp.state == UDHCP_BOUND) {
                uint8_t mac[6];
                for (int b = 0; b < 6; b++) mac[b] = (uint8_t)(d.mac >> (b * 8));
                udhcp_init(&c->dhcp, c->name, mac);
            }

            // CARRIER IS POLLED, NEVER WAITED ON. udhcp's own wait is
            // ten seconds, which is right for a command and would here
            // stall every other card behind a port with no cable in it.
            // A driver that cannot report carrier is not "down".
            if (d.link_known && !d.link_up) continue;
            if (d.ip && c->dhcp.state == UDHCP_INIT && !c->dhcp.lease.seconds && !c->forced)
                continue;   // addressed by hand: leave it alone

            uint64_t due = udhcp_step(&c->dhcp, sys_monotonic_ns());
            if (c->dhcp.state == UDHCP_BOUND) c->forced = 0;
            if (due < next) next = due;
        }

        // Never longer than ten seconds even with nothing due, because a
        // card appearing or a cable going in is not an event this can
        // wait on -- there is no hot-plug notification to ring 3.
        uint64_t now = sys_monotonic_ns();
        uint64_t ms = next > now ? (next - now) / 1000000ull : 0;
        if (ms > 10000) ms = 10000;
        if (g_chan_open) uchan_server_wait(&g_chan, (int)(ms ? ms : 100));
        else sys_sleep_ms((int)(ms ? ms : 100));
    }
    return 0;
}
