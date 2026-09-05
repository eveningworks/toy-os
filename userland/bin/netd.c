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
static int rule_name(const struct etc_config_buf *buf,
                     const struct query_netdev *d, char *out, uint32_t cap) {
    char key[24];
    mac_str(key, d->mac);
    if (etc_config_buf_get(buf, key, out, cap) && out[0]) return 1;
    if (d->location[0] && etc_config_buf_get(buf, d->location, out, cap) && out[0])
        return 1;
    if (etc_config_buf_get(buf, d->driver, out, cap) && out[0]) return 1;

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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    udhcp_log_to_kernel(1);   // a service's fd 1 reaches nobody

    // STATIC, NOT A LOCAL. etc_config_buf is 4 KiB and a ring-3 frame is
    // budgeted at 2 KiB against a single guard page, so this on the
    // stack does not merely overflow -- it steps over the guard into
    // unmapped space. -Wframe-larger-than catches it; the fix is that
    // this daemon has one thread and one loop.
    static struct etc_config_buf conf;

    for (;;) {
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

            if (strcmp(want_dhcp, "all") != 0) continue;

            // CARRIER IS POLLED, NEVER WAITED ON. udhcp's own wait is
            // ten seconds, which is right for a command and would here
            // stall every other card behind a port with no cable in it.
            // A driver that cannot report carrier is not "down".
            if (d.link_known && !d.link_up) continue;
            if (d.ip && c->dhcp.state == UDHCP_INIT && !c->dhcp.lease.seconds)
                continue;   // addressed by hand: leave it alone

            uint64_t due = udhcp_step(&c->dhcp, sys_monotonic_ns());
            if (due < next) next = due;
        }

        // Never longer than ten seconds even with nothing due, because a
        // card appearing or a cable going in is not an event this can
        // wait on -- there is no hot-plug notification to ring 3.
        uint64_t now = sys_monotonic_ns();
        uint64_t ms = next > now ? (next - now) / 1000000ull : 0;
        if (ms > 10000) ms = 10000;
        sys_sleep_ms((int)(ms ? ms : 100));
    }
    return 0;
}
