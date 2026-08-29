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

    for (int waited = 0; waited < WAIT_MS; waited += POLL_MS) {
        uint32_t src = 0;
        uint16_t port = 0;
        int64_t n = sys_recvfrom(fd, in, sizeof *in, &src, &port);
        if (n > 0) {
            uint32_t len = (uint32_t)n;
            if (in->op == OP_REPLY && !memcmp(in->xid, xid, 4) &&
                get32(in->magic) == DHCP_MAGIC) {
                const uint8_t *v = 0;
                if (option_get(in, len, OPT_MSG_TYPE, &v) >= 1 && *v == want) return (int)len;
            }
            continue;   // somebody else's traffic; keep waiting
        }
        sys_sleep_ms(POLL_MS);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) {
        cmd_usage("dhcp [<device>]");
        return 1;
    }

    // Which card, and its MAC. Both come from the same record, so there
    // is no window where the name and the hardware address disagree.
    struct query_netdev dev;
    int found = 0;
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_NETDEV, i, &dev, sizeof dev) < (int)sizeof dev) break;
        if (argc == 1 || !strcmp(dev.name, argv[1])) { found = 1; break; }
    }
    if (!found) {
        printf("dhcp: no such device%s%s\n", argc > 1 ? ": " : "", argc > 1 ? argv[1] : "");
        return 1;
    }

    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(dev.mac >> (i * 8));

    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) { cmd_fail("dhcp", "socket"); return 1; }
    // Bound to the CARD as well as the port: a second interface's
    // server must not answer this one's request, which is the whole
    // reason SO_BINDTODEVICE exists on Linux and why bind takes a name.
    if (sys_bind(fd, 0, DHCP_CLIENT_PORT, dev.name) < 0) {
        cmd_fail("dhcp", "bind");
        sys_close(fd);
        return 1;
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
        printf("dhcp: no offer on %s\n", dev.name);
        sys_close(fd);
        return 1;
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
    if (!len) {
        printf("dhcp: %s offered %u.%u.%u.%u and did not acknowledge it\n",
               dev.name, (l.ip >> 24) & 0xFF, (l.ip >> 16) & 0xFF,
               (l.ip >> 8) & 0xFF, l.ip & 0xFF);
        sys_close(fd);
        return 1;
    }
    // The ACK is authoritative, not the offer: a server may acknowledge
    // something other than what it offered.
    l.ip = get32(in.yiaddr);
    if (option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK)) l.mask = option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK);
    if (option_ip(&in, (uint32_t)len, OPT_ROUTER)) l.router = option_ip(&in, (uint32_t)len, OPT_ROUTER);
    if (option_ip(&in, (uint32_t)len, OPT_DNS)) l.dns = option_ip(&in, (uint32_t)len, OPT_DNS);
    l.seconds = option_ip(&in, (uint32_t)len, OPT_LEASE_TIME);
    sys_close(fd);

    if (sys_net_config(dev.name, l.ip, l.mask, l.router) < 0) {
        cmd_fail("dhcp", dev.name);
        return 1;
    }

    char a[20], m[20], g[20], d[20];
    ip_str(a, l.ip); ip_str(m, l.mask); ip_str(g, l.router); ip_str(d, l.dns);
    printf("dhcp: %s: %s netmask %s gateway %s\n", dev.name, a, m, g);

    if (l.dns) {
        // The FILE is Unix's name and the FORMAT is this repo's
        // `key=value` -- there is one config parser here and a second
        // one for four bytes would be the drift nobody looks for.
        if (uconf_set(RESOLV_CONF, "nameserver", d))
            printf("dhcp: nameserver %s -> %s\n", d, RESOLV_CONF);
        else
            printf("dhcp: could not write %s\n", RESOLV_CONF);
    }
    if (l.seconds) printf("dhcp: lease %u seconds (not renewed -- see the manual)\n", l.seconds);
    return 0;
}
