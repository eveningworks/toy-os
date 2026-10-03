// netctl -- the network cards: what they are, their addresses, and
// switching them. It replaced ifconfig, whose listing is `netctl status`
// and whose write half is `netctl address`.
//
// THE COUNTERS ARE THE DIAGNOSIS, which is why the listing prints them
// rather than hiding them behind a flag. A card with rx climbing and tx
// flat is listening to a network it cannot answer on; one with
// rx_dropped climbing alongside rx is being handed frames faster than
// the stack drains them. Neither is visible from `ping` alone.
//
// RENEW, DOWN AND UP ARE NETD'S (lib/unetctl.h), as systemd's
// `networkctl` asks networkd: netd owns the leases, so a card renewed
// behind its back would be re-leased, or left alone, by a daemon that
// did not know. The answer is "accepted"; the outcome is read back here
// from the card itself.
#include <stdint.h>
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>
#include "lib/uargs.h"
#include "lib/uchan.h"
#include "lib/uconf.h"
#include "lib/unetctl.h"

#define WAIT_MS   15000   // longer than one DHCP exchange's worst case
#define REPLY_MS  15000   // netd answers between passes, and a pass can be one exchange

static int g_no_wait;

static const struct uargs_opt OPTS[] = {
    { "no-wait", 0, 0, "renew, up: return once netd has the request", &g_no_wait, 0 },
    { 0 },
};

static const struct uargs_cmd CMDS[] = {
    { "status",  "[DEVICE]", "every card, or one: address, link, lease, counters (also a bare netctl)" },
    { "address", "DEVICE ADDRESS [NETMASK [GATEWAY]]", "set an address by hand" },
    { "renew",   "DEVICE",   "ask for the lease again, the same address first" },
    { "down",    "DEVICE",   "switch the card off: nothing sent or received, address cleared" },
    { "up",      "DEVICE",   "switch it back on and lease at once" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "netctl",
    .usage = "[OPTION]... [COMMAND [ARG]...]",
    .summary = "Show and control the network cards. renew, down and up are carried out\n"
               "by /bin/netd, which owns the leases.",
    .opts = OPTS,
    .cmds = CMDS,
    .notes = "DEVICE is a card's name as `netctl` lists it (net-718ebf, or what\n"
             "/etc/net.conf calls it). Addresses are dotted quads.\n"
             "Exit status: 0 done, 1 refused or failed, 2 bad usage.",
};

static void print_ip(char *out, size_t cap, unsigned long long ip) {
    if (!ip) { snprintf(out, cap, "-"); return; }
    snprintf(out, cap, "%llu.%llu.%llu.%llu",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// Dotted quad to a host-order address. Returns 0 on anything that is
// not exactly four numbers under 256 -- "10.0.2" silently meaning
// 10.0.0.2 is the classic way to configure the wrong subnet.
static int parse_ip(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned octet = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || octet > 255) return 0;
        }
        v = (v << 8) | octet;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

static void link_words(const struct query_netdev *d, char *out, size_t cap) {
    if (!d->link_up)
        snprintf(out, cap, "down");
    else if (d->link_bps >= 1000000000ULL && d->link_bps % 1000000000ULL)
        snprintf(out, cap, "up, %llu.%llu Gb/s",   // 2.5 Gb/s is not 2
                 (unsigned long long)(d->link_bps / 1000000000ULL),
                 (unsigned long long)(d->link_bps % 1000000000ULL / 100000000ULL));
    else if (d->link_bps >= 1000000000ULL)
        snprintf(out, cap, "up, %llu Gb/s", (unsigned long long)(d->link_bps / 1000000000ULL));
    else if (d->link_bps >= 1000000ULL)
        snprintf(out, cap, "up, %llu Mb/s", (unsigned long long)(d->link_bps / 1000000ULL));
    else if (d->link_bps)
        snprintf(out, cap, "up, %llu bit/s", (unsigned long long)d->link_bps);
    else
        snprintf(out, cap, "up");
}

static void show_one(const struct query_netdev *d) {
    char ip[24], mask[24], gw[24];
    print_ip(ip, sizeof ip, d->ip);
    print_ip(mask, sizeof mask, d->netmask);
    print_ip(gw, sizeof gw, d->gateway);

    // WHERE the card is, beside what it is called. The name is an
    // identity that follows the card; this is the changeable half. For
    // USB it is the controller's port, not the socket (docs/bugs.md).
    char at[20] = "";
    if (d->location[0]) snprintf(at, sizeof at, "  at %s", d->location);
    printf("%s: %s  %02x:%02x:%02x:%02x:%02x:%02x%s  mtu %llu\n",
           d->name, d->driver,
           (unsigned)(d->mac & 0xFF), (unsigned)((d->mac >> 8) & 0xFF),
           (unsigned)((d->mac >> 16) & 0xFF), (unsigned)((d->mac >> 24) & 0xFF),
           (unsigned)((d->mac >> 32) & 0xFF), (unsigned)((d->mac >> 40) & 0xFF),
           at, (unsigned long long)d->mtu);
    if (d->admin_down) printf("    switched off (netctl up %s)\n", d->name);
    if (d->ip) printf("    inet %s  netmask %s  gateway %s\n", ip, mask, gw);
    else       printf("    inet (unconfigured)\n");

    // LINK, and only when the driver can actually answer: a card with no
    // way to ask is not a card whose cable is unplugged.
    if (d->link_known) {
        char speed[32];
        link_words(d, speed, sizeof speed);
        printf("    link %s\n", speed);
    }

    // THE LEASE netd holds, from the file it keeps -- which survives a
    // `down`, so it is the address `up` will ask for first.
    char path[64], secs[16], server[24];
    snprintf(path, sizeof path, "/var/dhcp-%s.lease", d->name);
    if (uconf_get(path, "seconds", secs, sizeof secs) && secs[0] &&
        uconf_get(path, "server", server, sizeof server)) {
        unsigned long s = 0;
        for (const char *p = secs; *p >= '0' && *p <= '9'; p++) s = s * 10 + (unsigned long)(*p - '0');
        if (s % 3600 == 0) printf("    lease %lu h from %s\n", s / 3600, server);
        else               printf("    lease %lu s from %s\n", s, server);
    }

    printf("    rx %llu packets, %llu bytes, %llu dropped\n"
           "    tx %llu packets, %llu bytes, %llu dropped\n",
           (unsigned long long)d->rx_packets, (unsigned long long)d->rx_bytes,
           (unsigned long long)d->rx_dropped,
           (unsigned long long)d->tx_packets, (unsigned long long)d->tx_bytes,
           (unsigned long long)d->tx_dropped);
}

static int find(const char *name, struct query_netdev *out) {
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i)
        if (!strcmp(d.name, name)) { *out = d; return 1; }
    return 0;
}

static int cmd_status(const char *only) {
    int n = 0;
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (only && strcmp(d.name, only)) continue;
        show_one(&d);
        n++;
    }
    if (only && !n) { printf("netctl: no such device: %s\n", only); return 1; }
    // Never a silent empty listing: a machine with no card is an
    // ordinary state, and saying so differs from the command failing.
    if (!n) printf("no network devices\n");
    return 0;
}

static int cmd_address(char **v, int n) {
    uint32_t ip = 0, mask = 0, gw = 0;
    if (!parse_ip(v[1], &ip))            { printf("netctl: not an address: %s\n", v[1]); return 1; }
    if (n > 2 && !parse_ip(v[2], &mask)) { printf("netctl: not a netmask: %s\n", v[2]); return 1; }
    if (n > 3 && !parse_ip(v[3], &gw))   { printf("netctl: not a gateway: %s\n", v[3]); return 1; }
    if (sys_net_config(v[0], ip, mask, gw) < 0) { printf("netctl: no such device: %s\n", v[0]); return 1; }
    return cmd_status(v[0]);
}

// One request to netd: 1 and its result; -1 when netd has no channel up
// (not running); 0 when it did not ANSWER in time -- which is not the
// same thing: the request is in its ring and it will still act on it,
// so falling back to the syscall then would apply it twice.
static int ask_netd(uint32_t verb, const char *dev, uint32_t *result) {
    struct uchan_client c;
    if (uchan_client_open(&c, NETCTL_SERVICE) < 0) return -1;
    struct netctl_msg m, reply;
    memset(&m, 0, sizeof m);
    m.verb = verb;
    snprintf(m.dev, sizeof m.dev, "%s", dev);
    int ok = uchan_call(&c, &m, sizeof m, &reply, sizeof reply, REPLY_MS) == 0;
    uchan_client_close(&c);
    if (ok) *result = reply.result;
    return ok;
}

// When netd's last ACK for this card came (the lease file's `acked`,
// udhcp.c); 0 when there is no lease.
static int lease_stamp(const char *dev, char *t, int cap) {
    char path[64];
    snprintf(path, sizeof path, "/var/dhcp-%s.lease", dev);
    t[0] = 0;
    return uconf_get(path, "acked", t, (uint32_t)cap) && t[0];
}

// Polls until the card has an address the lease gave it -- and, when
// there was a lease before (`had`), until netd has WRITTEN A NEW ONE:
// a renew keeps the same address, so "it has an address" would report
// success at once whether or not the renew ever happened. Returns 1
// with the address, 0 when the wait ran out.
static int wait_for_lease(const char *dev, struct query_netdev *d, int had,
                          const char *before) {
    unsigned long long until = sys_monotonic_ns() + (unsigned long long)WAIT_MS * 1000000ull;
    while (sys_monotonic_ns() < until) {
        char now[24];
        int fresh = !had || (lease_stamp(dev, now, sizeof now) && strcmp(now, before) != 0);
        if (fresh && find(dev, d) && d->ip && (d->ip >> 16) != 0xA9FE) return 1;   // not 169.254/16
        sys_sleep_ms(250);
    }
    return 0;
}

static int cmd_control(uint32_t verb, const char *dev) {
    struct query_netdev d;
    if (!find(dev, &d)) { printf("netctl: no such device: %s\n", dev); return 1; }
    uint32_t r = 0;
    char before[24];
    int had = lease_stamp(dev, before, sizeof before);
    int asked = ask_netd(verb, dev, &r);
    if (asked == 0) {
        printf("netctl: netd did not answer in %d s -- it is busy, and will still act on it\n",
               REPLY_MS / 1000);
        return 1;
    }
    if (asked < 0) {
        // NO NETD: down and up are the kernel's switch and still work;
        // renew has nobody to lease.
        if (verb == NETCTL_RENEW) { printf("netctl: netd is not running -- nothing to renew with\n"); return 1; }
        unsigned f = verb == NETCTL_DOWN ? NET_IFC_DOWN : NET_IFC_UP;
        if (sys_net_admin(dev, f) < 0) { printf("netctl: %s refused it\n", dev); return 1; }
        printf("netctl: %s is %s (netd is not running, so nothing will lease it)\n",
               dev, verb == NETCTL_DOWN ? "down" : "up");
        return 0;
    }
    if (r == NETCTL_NO_SUCH) { printf("netctl: netd does not know %s yet\n", dev); return 1; }
    if (r != NETCTL_OK) {
        printf("netctl: netd refused%s\n", verb == NETCTL_RENEW ? " -- the card is down (netctl up)" : "");
        return 1;
    }
    if (verb == NETCTL_DOWN) { printf("%s: down\n", dev); return 0; }
    if (g_no_wait) { printf("%s: asked\n", dev); return 0; }
    // UP ON A CARD NOBODY LEASES (addressed by hand, or dhcp = no): it
    // kept its address through `down`, and there is no lease to wait on.
    if (verb == NETCTL_UP && !had && find(dev, &d) && d.ip && (d.ip >> 16) != 0xA9FE) {
        char ip[24];
        print_ip(ip, sizeof ip, d.ip);
        printf("%s: %s\n", dev, ip);
        return 0;
    }
    if (!wait_for_lease(dev, &d, had, before)) {
        printf("%s: no new lease yet -- netd keeps asking (netctl status %s)\n", dev, dev);
        return 1;
    }
    char ip[24];
    print_ip(ip, sizeof ip, d.ip);
    printf("%s: %s\n", dev, ip);
    return 0;
}

static int wrong(const char *cmd) {
    for (const struct uargs_cmd *c = CMDS; c->name; c++)
        if (!strcmp(c->name, cmd))
            return uargs_error(&PROG, "'%s' takes %s", cmd, c->args);
    return uargs_error(&PROG, "unknown command '%s'", cmd);
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    const char *cmd = a.argc ? a.argv[0] : "status";
    char **rest = a.argv + 1;
    int n = a.argc ? a.argc - 1 : 0;

    if (!strcmp(cmd, "status"))  return n > 1 ? wrong(cmd) : cmd_status(n ? rest[0] : 0);
    if (!strcmp(cmd, "address")) return n < 2 || n > 4 ? wrong(cmd) : cmd_address(rest, n);
    if (!strcmp(cmd, "renew"))   return n != 1 ? wrong(cmd) : cmd_control(NETCTL_RENEW, rest[0]);
    if (!strcmp(cmd, "down"))    return n != 1 ? wrong(cmd) : cmd_control(NETCTL_DOWN, rest[0]);
    if (!strcmp(cmd, "up"))      return n != 1 ? wrong(cmd) : cmd_control(NETCTL_UP, rest[0]);
    return wrong(cmd);
}
