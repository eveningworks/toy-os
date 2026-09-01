// dhcp -- ask the network for an address instead of inventing one.
//
// WHY THIS IS A RING-3 PROGRAM. DHCP is policy: which offer to take,
// how long to wait, what to do with the lease, where to write the
// resolver. None of that is the kernel's business, and the kernel has
// no business growing a state machine that talks to whoever answers a
// broadcast first. It applies its result through SYS_NET_CONFIG, the
// same call `ifconfig` uses, so there is no privileged path here that
// a person could not take by hand.
//
// NO SERVER IS NOT A FAILURE: a device nobody offers a lease to claims
// a link-local address instead (RFC 3927, Windows' APIPA), so a machine
// on a segment with no DHCP server can still talk to its neighbours.
// The kernel invents no address at all now -- this program is where one
// comes from.
//
// WITH NO ARGUMENT IT CONFIGURES EVERY DEVICE THAT HAS NO ADDRESS,
// which is what dhclient does when no interface is named. Naming one
// takes that device whatever state it is in, which is how a card is
// re-leased by hand.
//
// THE LEASE IS NOT RENEWED. A real client keeps a timer and renews at
// T1 (half the lease); this asks once and applies what it gets. That is
// a real limitation rather than a simplification: a lease that expires
// under a long-running machine leaves it using an address the server
// has since given away. Renewal needs a daemon, and a daemon needs a
// reason to exist beyond one timer -- see docs/roadmap.md.
//
// A DISCOVER IS SENT WITH THE BROADCAST FLAG SET, so the reply comes
// back to 255.255.255.255 rather than to an address this machine does
// not have yet. Some servers honour the flag and some unicast anyway
// (to the offered address, at the client's MAC); the socket accepts
// both, because kernel/net/ipv4.c takes a broadcast on any device and a
// unicast on a device that already has that address.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "lib/cmd.h"
#include "lib/uconf.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u

#define OP_REQUEST 1
#define OP_REPLY   2
#define HTYPE_ETHERNET 1

#define MSG_DISCOVER 1
#define MSG_OFFER    2
#define MSG_REQUEST  3
#define MSG_ACK      5
#define MSG_NAK      6

#define OPT_SUBNET_MASK 1
#define OPT_ROUTER      3
#define OPT_DNS         6
#define OPT_REQUESTED   50
#define OPT_LEASE_TIME  51
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_PARAM_LIST  55
#define OPT_END         255

#define FLAG_BROADCAST 0x8000

#define WAIT_MS 4000
#define POLL_MS 10

#define RESOLV_CONF "/etc/resolv.conf"

// RFC 3927 link-local, the fallback when nothing answers a DISCOVER.
// The usable range excludes the first and last /24, which the RFC
// reserves; the counts and the spacing are its own (PROBE_NUM,
// MAX_CONFLICTS, ANNOUNCE_NUM).
#define LL_FIRST      0xA9FE0100u     // 169.254.1.0
#define LL_COUNT      65024u          // ... through 169.254.254.255
#define LL_MASK       0xFFFF0000u
#define LL_TRIES      10
#define LL_PROBES     3
#define LL_PROBE_MS   1000
#define LL_ANNOUNCE   2
#define LL_ANNOUNCE_MS 2000

// The fixed part of a BOOTP message, which DHCP is options bolted onto.
// Every multi-byte field is big-endian on the wire; the helpers below
// are what keep that from being a per-field decision.
struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint8_t xid[4];
    uint8_t secs[2], flags[2];
    uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
    uint8_t options[312];
};

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void ip_str(char *out, uint32_t ip) {
    snprintf(out, 20, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// One option's value. Walks the option area with LENGTH CHECKS at every
// step: this is a parser reading whatever answered a broadcast, so a
// truncated or lying length is the ordinary case to handle rather than
// an anomaly. Returns the value length, or 0.
static uint32_t option_get(const struct dhcp_msg *m, uint32_t msg_len,
                           uint8_t want, const uint8_t **out) {
    if (msg_len < sizeof *m - sizeof m->options) return 0;
    uint32_t avail = msg_len - (uint32_t)(sizeof *m - sizeof m->options);
    if (avail > sizeof m->options) avail = sizeof m->options;

    for (uint32_t i = 0; i < avail; ) {
        uint8_t code = m->options[i];
        if (code == OPT_END) return 0;
        if (code == 0) { i++; continue; }        // pad
        if (i + 2 > avail) return 0;
        uint8_t len = m->options[i + 1];
        if (i + 2 + len > avail) return 0;
        if (code == want) {
            *out = &m->options[i + 2];
            return len;
        }
        i += 2u + len;
    }
    return 0;
}

static uint32_t option_ip(const struct dhcp_msg *m, uint32_t len, uint8_t want) {
    const uint8_t *v = 0;
    return option_get(m, len, want, &v) >= 4 ? get32(v) : 0;
}

struct lease {
    uint32_t ip, mask, router, dns, server, seconds;
};

static void build(struct dhcp_msg *m, const uint8_t *mac, const uint8_t *xid,
                  uint8_t type, uint32_t requested, uint32_t server, uint32_t *out_len) {
    memset(m, 0, sizeof *m);
    m->op = OP_REQUEST;
    m->htype = HTYPE_ETHERNET;
    m->hlen = 6;
    memcpy(m->xid, xid, 4);
    m->flags[0] = (uint8_t)(FLAG_BROADCAST >> 8);
    memcpy(m->chaddr, mac, 6);
    put32(m->magic, DHCP_MAGIC);

    uint32_t i = 0;
    m->options[i++] = OPT_MSG_TYPE; m->options[i++] = 1; m->options[i++] = type;
    if (requested) {
        m->options[i++] = OPT_REQUESTED; m->options[i++] = 4;
        put32(&m->options[i], requested); i += 4;
    }
    if (server) {
        m->options[i++] = OPT_SERVER_ID; m->options[i++] = 4;
        put32(&m->options[i], server); i += 4;
    }
    // Asking for what we intend to use, which is what a server keys its
    // reply on -- an offer need not carry an option nobody requested.
    m->options[i++] = OPT_PARAM_LIST; m->options[i++] = 3;
    m->options[i++] = OPT_SUBNET_MASK;
    m->options[i++] = OPT_ROUTER;
    m->options[i++] = OPT_DNS;
    m->options[i++] = OPT_END;

    *out_len = (uint32_t)(sizeof *m - sizeof m->options) + i;
}

// Send, then wait for a reply of the expected type carrying our xid.
// Anything else on the wire is ignored rather than treated as an error:
// a broadcast port hears every client on the segment.
static int exchange(int fd, struct dhcp_msg *out, uint32_t out_len,
                    const uint8_t *xid, uint8_t want, struct dhcp_msg *in) {
    int64_t rc = -1;
    for (int waited = 0; waited < WAIT_MS; waited += POLL_MS) {
        rc = sys_sendto(fd, out, out_len, 0xFFFFFFFFu, DHCP_SERVER_PORT);
        if (rc >= 0 || sys_errno() != EAGAIN) break;
        sys_sleep_ms(POLL_MS);
    }
    if (rc < 0) return 0;

    // A blocking receive, but still a LOOP: a broadcast port hears
    // every client on the segment, so a datagram that is not ours is
    // ignored and the wait continues on what is LEFT of the budget --
    // recomputed each time, or somebody else's traffic would extend our
    // deadline indefinitely.
    uint64_t deadline = sys_monotonic_ns() + (uint64_t)WAIT_MS * 1000000ull;
    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= deadline) return 0;
        unsigned left = (unsigned)((deadline - now) / 1000000ull);

        uint32_t src = 0;
        uint16_t port = 0;
        int64_t n = sys_recvfrom(fd, in, sizeof *in, &src, &port, left ? left : 1);
        if (n <= 0) return 0;   // the budget expired

        uint32_t len = (uint32_t)n;
        if (in->op == OP_REPLY && !memcmp(in->xid, xid, 4) &&
            get32(in->magic) == DHCP_MAGIC) {
            const uint8_t *v = 0;
            if (option_get(in, len, OPT_MSG_TYPE, &v) >= 1 && *v == want) return (int)len;
        }
    }
}

// --- link-local ------------------------------------------------------

// A candidate address for this card. RFC 3927 asks for a pseudo-random
// choice SEEDED FROM THE HARDWARE ADDRESS, so a machine tends to pick
// the same address across reboots and its neighbours' caches stay true;
// `attempt` reseeds it after a collision. FNV-1a because it is four
// lines and this is not a security decision.
static uint32_t ll_candidate(const uint8_t *mac, int attempt) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    h ^= (uint32_t)attempt; h *= 16777619u;
    return LL_FIRST + (h % LL_COUNT);
}

// Is `ip` already somebody's? 1 yes, 0 no, -1 could not ask.
//
// One probe more than the RFC's three, because the syscall is
// edge-triggered: a reply is visible only to the call AFTER the one
// that provoked it, so the last iteration is a read. The extra frame is
// a probe like the others.
static int ll_taken(const char *dev, uint32_t ip) {
    for (int i = 0; i <= LL_PROBES; i++) {
        int r = sys_net_arp_probe(dev, ip);
        if (r != 0) return r > 0 ? 1 : -1;
        if (i < LL_PROBES) sys_sleep_ms(LL_PROBE_MS);
    }
    return 0;
}

// Claim an address nobody answers for. Returns 1 on success.
static int link_local(const char *dev, const uint8_t *mac) {
    for (int attempt = 0; attempt < LL_TRIES; attempt++) {
        uint32_t ip = ll_candidate(mac, attempt);
        int taken = ll_taken(dev, ip);
        // A REFUSAL IS NOT A COLLISION. Trying the next candidate after
        // one would spend ten more rounds of probes learning the same
        // thing and then report a full range instead of a dead device.
        if (taken < 0) { cmd_fail("dhcp", dev); return 0; }
        if (taken) continue;

        // No gateway: a link-local address routes to its own segment
        // and nowhere else, which is the whole of what it promises.
        if (sys_net_config(dev, ip, LL_MASK, 0) < 0) {
            cmd_fail("dhcp", dev);
            return 0;
        }

        char a[20];
        ip_str(a, ip);
        printf("dhcp: %s: %s netmask 255.255.0.0 link-local, no gateway\n", dev, a);

        // ANNOUNCE, so a neighbour that cached nothing during the
        // probes learns the address now. The request carries our new
        // address as its sender, which is what makes it an
        // announcement rather than another probe.
        for (int i = 0; i < LL_ANNOUNCE; i++) {
            sys_net_arp_probe(dev, ip);
            if (i + 1 < LL_ANNOUNCE) sys_sleep_ms(LL_ANNOUNCE_MS);
        }
        return 1;
    }
    printf("dhcp: %s: no free link-local address after %d tries\n", dev, LL_TRIES);
    return 0;
}

// --- carrier ----------------------------------------------------------
//
// WAIT FOR THE WIRE BEFORE ASKING, which is what every real client does
// -- systemd-networkd's ConfigureWithoutCarrier defaults to no, and
// dhcpcd will not send on a down link either.
//
// Until this existed the wait happened BY ACCIDENT: sendto() on a down
// link returns EAGAIN and exchange() retried for WAIT_MS, so the
// DISCOVER budget and the carrier wait were the same four seconds and
// ate each other. Measured on the bare-metal laptop, init starts this
// at ~1.45 s and the RTL8153's PHY reports link at 4.4-5.9 s, so the
// accidental wait expired at ~5.45 s -- a coin flip, which is exactly
// how it behaved: an address on some boots and link-local on others.
//
// A DRIVER THAT CANNOT REPORT CARRIER IS NOT "DOWN". link_known is 0
// for those, and waiting on an answer that will never come would turn
// a working card into a ten-second delay and then link-local.
#define CARRIER_WAIT_MS 10000
#define CARRIER_POLL_MS 100

static int wait_for_carrier(const char *name) {
    struct query_netdev d;
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_NETDEV, i, &d, sizeof d) < (int)sizeof d) return 1;
        if (!strcmp(d.name, name)) break;
    }
    if (!d.link_known) return 1;   // cannot be asked: proceed, do not stall
    if (d.link_up) return 1;

    printf("dhcp: %s: waiting for carrier...\n", name);
    for (int waited = 0; waited < CARRIER_WAIT_MS; waited += CARRIER_POLL_MS) {
        sys_sleep_ms(CARRIER_POLL_MS);
        for (unsigned i = 0; ; i++) {
            if (sys_query_record(QUERY_NETDEV, i, &d, sizeof d) < (int)sizeof d) break;
            if (strcmp(d.name, name)) continue;
            if (d.link_up) {
                printf("dhcp: %s: link up %uM after %d.%ds\n", name,
                       (unsigned)(d.link_bps / 1000000), waited / 1000,
                       (waited % 1000) / 100);
                return 1;
            }
            break;
        }
    }
    // Bounded, so an unplugged machine still reaches a usable state --
    // link-local, exactly as before, just after a wait that was worth
    // making.
    printf("dhcp: %s: no carrier after %ds\n", name, CARRIER_WAIT_MS / 1000);
    return 0;
}

// --- one device ------------------------------------------------------

// Returns 1 if the device ends up with an address, by lease or by
// claim.
static int configure(const struct query_netdev *dev, struct lease *got) {
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(dev->mac >> (i * 8));

    // Before the socket, before the DISCOVER: there is nothing to ask
    // on a dead wire, and asking anyway is what made this a coin flip.
    if (!wait_for_carrier(dev->name)) return link_local(dev->name, mac);

    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) { cmd_fail("dhcp", "socket"); return 0; }
    // Bound to the CARD as well as the port: a second interface's
    // server must not answer this one's request, which is the whole
    // reason SO_BINDTODEVICE exists on Linux and why bind takes a name.
    if (sys_bind(fd, 0, DHCP_CLIENT_PORT, dev->name) < 0) {
        cmd_fail("dhcp", "bind");
        close(fd);
        return 0;
    }

    uint8_t xid[4];
    if (sys_getrandom(xid, sizeof xid) != (int64_t)sizeof xid) {
        // A predictable xid is not a security problem here, but two
        // clients booting together with the SAME one would each take
        // the other's offer. Falling back to the clock keeps them apart.
        put32(xid, (uint32_t)sys_monotonic_ns());
    }

    static struct dhcp_msg out, in;
    uint32_t out_len = 0;
    build(&out, mac, xid, MSG_DISCOVER, 0, 0, &out_len);
    int len = exchange(fd, &out, out_len, xid, MSG_OFFER, &in);
    if (!len) {
        close(fd);
        printf("dhcp: no offer on %s\n", dev->name);
        return link_local(dev->name, mac);
    }

    struct lease l = {0};
    l.ip = get32(in.yiaddr);
    l.mask = option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK);
    l.router = option_ip(&in, (uint32_t)len, OPT_ROUTER);
    l.dns = option_ip(&in, (uint32_t)len, OPT_DNS);
    l.server = option_ip(&in, (uint32_t)len, OPT_SERVER_ID);

    // The REQUEST is what makes the offer a lease -- a client that
    // applied the offer without it is using an address the server still
    // considers free to hand to somebody else.
    build(&out, mac, xid, MSG_REQUEST, l.ip, l.server, &out_len);
    len = exchange(fd, &out, out_len, xid, MSG_ACK, &in);
    close(fd);
    if (!len) {
        char a[20];
        ip_str(a, l.ip);
        printf("dhcp: %s offered %s and did not acknowledge it\n", dev->name, a);
        return link_local(dev->name, mac);
    }
    // The ACK is authoritative, not the offer: a server may acknowledge
    // something other than what it offered.
    l.ip = get32(in.yiaddr);
    if (option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK)) l.mask = option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK);
    if (option_ip(&in, (uint32_t)len, OPT_ROUTER)) l.router = option_ip(&in, (uint32_t)len, OPT_ROUTER);
    if (option_ip(&in, (uint32_t)len, OPT_DNS)) l.dns = option_ip(&in, (uint32_t)len, OPT_DNS);
    l.seconds = option_ip(&in, (uint32_t)len, OPT_LEASE_TIME);

    if (sys_net_config(dev->name, l.ip, l.mask, l.router) < 0) {
        cmd_fail("dhcp", dev->name);
        return 0;
    }

    char a[20], m[20], g[20], d[20];
    ip_str(a, l.ip); ip_str(m, l.mask); ip_str(g, l.router); ip_str(d, l.dns);
    printf("dhcp: %s: %s netmask %s gateway %s\n", dev->name, a, m, g);

    if (l.dns) {
        // The FILE is Unix's name and the FORMAT is this repo's
        // `key=value` -- there is one config parser here and a second
        // one for four bytes would be the drift nobody looks for.
        if (uconf_set(RESOLV_CONF, "nameserver", d))
            printf("dhcp: nameserver %s -> %s\n", d, RESOLV_CONF);
        else
            printf("dhcp: could not write %s\n", RESOLV_CONF);
    }
    if (l.seconds) printf("dhcp: lease %u seconds\n", l.seconds);
    if (got) *got = l;
    return 1;
}

// --- renewal ----------------------------------------------------------
//
// RFC 2131's timers: at T1 (half the lease) the client REQUESTs its own
// address from the server that granted it, and on an ACK the clock
// starts again. If that fails it keeps trying, and at expiry it falls
// all the way back to a DISCOVER -- because an address whose lease has
// run out is not ours any more, whatever the interface still says.
//
// NOT A FULL STATE MACHINE. RFC 2131 distinguishes RENEWING (unicast to
// the server) from REBINDING (broadcast at T2), and this broadcasts
// throughout: the machine that motivated it has one server on one
// segment, and a REBIND that reaches the same server is answered the
// same way. Said here rather than left to be discovered.
static struct lease k_lease;
static char renew_dev[NET_ABI_NAME_MAX];

static void renew_forever(void) {
    for (;;) {
        uint32_t half = k_lease.seconds / 2;
        if (half < 30) half = 30;      // a very short lease must not spin
        sys_sleep_ms((int)(half * 1000));

        struct query_netdev dev;
        int found = 0;
        for (unsigned i = 0; ; i++) {
            if (sys_query_record(QUERY_NETDEV, i, &dev, sizeof dev) < (int)sizeof dev) break;
            if (!strcmp(dev.name, renew_dev)) { found = 1; break; }
        }
        if (!found) return;            // the card went away

        struct lease got = {0};
        if (configure(&dev, &got) && got.seconds) {
            k_lease = got;
        } else {
            // Keep the old timer and try again sooner rather than
            // giving up: a server that is briefly unreachable is the
            // ordinary case, and the address stays valid until expiry.
            printf("dhcp: %s: renewal failed, retrying\n", renew_dev);
            k_lease.seconds = k_lease.seconds > 120 ? 120 : k_lease.seconds;
        }
    }
}

int main(int argc, char **argv) {
    // `-1` IS THE OLD BEHAVIOUR, and it is what a person typing this at
    // a prompt wants: ask, apply, exit. The service descriptor leaves it
    // off so the boot-time one stays resident and renews.
    int oneshot = 0;
    const char *want = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-1")) oneshot = 1;
        else if (!want && argv[i][0] != '-') want = argv[i];
        else { cmd_usage("dhcp [-1] [<device>]"); return 1; }
    }

    int tried = 0, done = 0;
    for (unsigned i = 0; ; i++) {
        struct query_netdev dev;
        if (sys_query_record(QUERY_NETDEV, i, &dev, sizeof dev) < (int)sizeof dev) break;

        if (want) {
            if (strcmp(dev.name, want)) continue;
        } else if (dev.ip) {
            // Said out loud rather than skipped silently: on a boot
            // where one card is already configured this is the whole
            // difference between "nothing to do" and "nothing worked".
            printf("dhcp: %s already has an address -- leaving it\n", dev.name);
            continue;
        }
        tried++;
        struct lease got = {0};
        if (configure(&dev, &got)) {
            done++;
            // The FIRST device with a real lease is the one this stays
            // alive to renew. One renewer for one lease: a second card
            // would need its own timer, and this machine has never had
            // two leases at once.
            if (!renew_dev[0] && got.seconds && got.server) {
                k_lease = got;
                strncpy(renew_dev, dev.name, sizeof renew_dev - 1);
            }
        }
    }

    if (!tried) {
        if (want) printf("dhcp: no such device: %s\n", want);
        else           printf("dhcp: no device without an address\n");
        return 1;
    }
    if (done != tried) return 1;

    // --- RENEWING (RFC 2131) ------------------------------------------
    //
    // Only when asked to: `-1` is the one-shot the boot used to be, and
    // is what a person typing `dhcp net0` at a prompt wants. As a
    // service this stays resident, because a lease that is never renewed
    // silently expires and the machine loses its address at an hour a
    // server chose.
    if (oneshot || !renew_dev[0]) return 0;
    renew_forever();
    return 0;
}
