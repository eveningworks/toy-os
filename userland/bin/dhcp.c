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
// THE LEASE IS RENEWED, as RFC 2131's three states rather than by
// asking again from scratch. At T1 it REQUESTs its own address, UNICAST
// to the server that granted it, with ciaddr set and neither a
// server-id nor a requested-IP option; at T2 it broadcasts the same
// message; only at expiry does it give the address up. The distinction
// is the whole point -- a broadcast DISCOVER says "I have no address",
// and a server that cannot reuse an outstanding lease answers it with a
// DIFFERENT one, which is how this machine's address stepped .104 ->
// .105 -> .106 in one afternoon without rebooting.
//
// THE LEASE IS ALSO REMEMBERED, so a reboot asks for the address it had
// (INIT-REBOOT: broadcast REQUEST, ciaddr 0, requested-IP set) instead
// of taking whatever is free.
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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

// --- where the output goes ---------------------------------------------
//
// A SERVICE HAS NO STDOUT ANYBODY READS. init spawns this with fd 1
// going nowhere, so every printf here reached exactly nothing -- which
// is how a boot that fell back to link-local left no record of WHY, and
// why the first diagnosis of it had to be done by adding up timings.
// tftpd.c carries the same note and solved it the same way.
//
// fd 2 IS THE KERNEL LOG here (abi/syscall_abi.h), so a diagnostic
// written there lands in `dmesg`. At a prompt that is the wrong place --
// the person typing `dhcp net0` wants to see the answer -- so this
// picks, and WHAT IT PICKS ON IS `-k`, not isatty().
//
// isatty(1) was the first version and does not work: init hands a
// service a console fd 1, so it answers TRUE and the boot's diagnostics
// went to a screen nothing presents on a graphical boot. Measured -- the
// laptop's dmesg carried `init: started dhcp` and not one line from dhcp
// itself. `-k` is the flag that MEANS "I am the resident service", so it
// is the thing that already knows the answer.
static int g_to_log;

static void say(const char *fmt, ...) {
    va_list ap;
    char buf[160];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write(g_to_log ? 2 : 1, buf, strlen(buf));
}


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
#define OPT_T1          58
#define OPT_T2          59
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_PARAM_LIST  55
#define OPT_END         255

#define FLAG_BROADCAST 0x8000
#define BCAST 0xFFFFFFFFu

#define WAIT_MS 4000
#define RETX_MS 1000   // first retransmit, then doubling -- RFC 2131 4.1
#define POLL_MS 10

#define RESOLV_CONF "/etc/resolv.conf"

// A LEASE IS STATE, NOT CONFIG, so it lives under /var rather than /etc
// -- the split the FHS makes and dhclient follows with
// `/var/lib/dhcp/dhclient.leases`. Flat rather than nested because
// creating a missing parent is a per-level mkdir here (see the note in
// save_lease()), and one supervised card means one file.
// PER DEVICE, because with no argument this configures every unaddressed
// card and only the FIRST is supervised: one shared file would be owned
// by whichever card was configured last, and the supervised card's
// INIT-REBOOT would then never match.
#define LEASE_DIR  "/var"
#define LEASE_MAX  32

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
    // RFC 2131's renewal and rebinding times, in seconds from the ACK.
    // The SERVER may state them (options 58 and 59) and its numbers win;
    // 1/2 and 7/8 are only the defaults for when it does not, which is
    // the part that reads as "the percentages" and is not the rule.
    uint32_t t1, t2;
};

// Fills t1/t2 from the server's options, or from the lease length.
static void lease_timers(struct lease *l, const struct dhcp_msg *m, uint32_t len) {
    l->t1 = option_ip(m, len, OPT_T1);
    l->t2 = option_ip(m, len, OPT_T2);
    if (!l->t1) l->t1 = l->seconds / 2;
    if (!l->t2) l->t2 = l->seconds - l->seconds / 8;   // 7/8, without overflow
    // A server that states them inconsistently is not worth honouring
    // into a state machine that then cannot leave RENEWING.
    if (l->t2 <= l->t1 || l->t2 > l->seconds) l->t2 = l->seconds - l->seconds / 8;
    if (l->t1 >= l->t2) l->t1 = l->t2 / 2;
}

// `ciaddr` is what separates a renewal from an acquisition: RFC 2131's
// table (4.3.6) says RENEWING and REBINDING carry the address in
// ciaddr and MUST NOT send a server-id or a requested-IP option, while
// SELECTING sends both and leaves ciaddr zero. Sending the wrong shape
// is not a protocol nicety -- a server reading "I have no address"
// allocates a new one.
//
// THE BROADCAST FLAG IS FOR A CLIENT THAT CANNOT YET RECEIVE UNICAST.
// Once ciaddr is set the interface has that address and the reply can
// come straight to it, so the flag is cleared -- setting it anyway asks
// every server on the segment to shout the answer.
static void build(struct dhcp_msg *m, const uint8_t *mac, const uint8_t *xid,
                  uint8_t type, uint32_t requested, uint32_t server,
                  uint32_t ciaddr, uint32_t *out_len) {
    memset(m, 0, sizeof *m);
    m->op = OP_REQUEST;
    m->htype = HTYPE_ETHERNET;
    m->hlen = 6;
    memcpy(m->xid, xid, 4);
    if (!ciaddr) m->flags[0] = (uint8_t)(FLAG_BROADCAST >> 8);
    else put32(m->ciaddr, ciaddr);
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
    m->options[i++] = OPT_PARAM_LIST; m->options[i++] = 5;
    m->options[i++] = OPT_SUBNET_MASK;
    m->options[i++] = OPT_ROUTER;
    m->options[i++] = OPT_DNS;
    m->options[i++] = OPT_T1;
    m->options[i++] = OPT_T2;
    m->options[i++] = OPT_END;

    *out_len = (uint32_t)(sizeof *m - sizeof m->options) + i;
}

// Send, then wait for a reply of the expected type carrying our xid.
// Anything else on the wire is ignored rather than treated as an error:
// a broadcast port hears every client on the segment.
// `dest` is 0xFFFFFFFF for everything except a RENEWING request, which
// goes straight to the server that granted the lease.
// Sends once, answering a busy socket. Returns 0 when the datagram
// could not be handed to the stack at all.
static int send_once(int fd, struct dhcp_msg *out, uint32_t out_len,
                     uint32_t dest) {
    for (int waited = 0; waited < WAIT_MS; waited += POLL_MS) {
        int64_t rc = sys_sendto(fd, out, out_len, dest, DHCP_SERVER_PORT);
        if (rc >= 0) return 1;
        if (sys_errno() != EAGAIN) return 0;
        sys_sleep_ms(POLL_MS);
    }
    return 0;
}

static int exchange(int fd, struct dhcp_msg *out, uint32_t out_len,
                    const uint8_t *xid, uint8_t want, struct dhcp_msg *in,
                    uint32_t dest) {
    // IT RETRANSMITS, which RFC 2131 4.1 requires and the first version
    // did not do: it sent ONE datagram and then only waited. A lost
    // request was therefore indistinguishable from "no server", and on
    // the bare-metal laptop that lost the boot's INIT-REBOOT every time
    // -- the switch port reports link before it forwards (MAC learning,
    // and spanning tree if enabled), so the first datagram out of a
    // freshly-carrier-up interface goes nowhere. The DISCOVER that
    // followed 4 s later always worked, which is what made this look
    // like a property of INIT-REBOOT rather than of the wire.
    //
    // The BUDGET is unchanged -- three attempts inside the same WAIT_MS
    // rather than one -- so a network with genuinely no server costs
    // exactly what it did before.
    uint64_t deadline = sys_monotonic_ns() + (uint64_t)WAIT_MS * 1000000ull;
    uint64_t next_send = 0;                 // 0 == send immediately
    uint32_t backoff_ms = RETX_MS;

    // A blocking receive, but still a LOOP: a broadcast port hears
    // every client on the segment, so a datagram that is not ours is
    // ignored and the wait continues on what is LEFT of the budget --
    // recomputed each time, or somebody else's traffic would extend our
    // deadline indefinitely.
    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= deadline) return 0;

        if (now >= next_send) {
            if (!send_once(fd, out, out_len, dest)) return 0;
            next_send = now + (uint64_t)backoff_ms * 1000000ull;
            // Doubling, as RFC 2131 asks. No randomisation: that exists
            // to stop a fleet of clients synchronising after a power
            // cut, and this is one machine with one interface.
            backoff_ms *= 2;
        }

        uint64_t until = next_send < deadline ? next_send : deadline;
        unsigned left = (unsigned)((until - now) / 1000000ull);

        uint32_t src = 0;
        uint16_t port = 0;
        int64_t n = sys_recvfrom(fd, in, sizeof *in, &src, &port, left ? left : 1);
        if (n <= 0) continue;   // this slice expired; retransmit or give up

        uint32_t len = (uint32_t)n;
        if (in->op == OP_REPLY && !memcmp(in->xid, xid, 4) &&
            get32(in->magic) == DHCP_MAGIC) {
            const uint8_t *v = 0;
            if (option_get(in, len, OPT_MSG_TYPE, &v) >= 1) {
                if (*v == want) return (int)len;
                // A NAK is the server saying "that binding is gone",
                // which is NOT the same as silence: RFC 2131 4.4.5 sends
                // the client straight back to INIT rather than letting
                // it keep the address until expiry. Reported as -1 so
                // the caller can tell the two apart.
                if (*v == MSG_NAK) return -1;
            }
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
        say("dhcp: %s: %s netmask 255.255.0.0 link-local, no gateway\n", dev, a);

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
    say("dhcp: %s: no free link-local address after %d tries\n", dev, LL_TRIES);
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

    say("dhcp: %s: waiting for carrier...\n", name);
    for (int waited = 0; waited < CARRIER_WAIT_MS; waited += CARRIER_POLL_MS) {
        sys_sleep_ms(CARRIER_POLL_MS);
        for (unsigned i = 0; ; i++) {
            if (sys_query_record(QUERY_NETDEV, i, &d, sizeof d) < (int)sizeof d) break;
            if (strcmp(d.name, name)) continue;
            if (d.link_up) {
                say("dhcp: %s: link up %uM after %d.%ds\n", name,
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
    say("dhcp: %s: no carrier after %ds\n", name, CARRIER_WAIT_MS / 1000);
    return 0;
}

// --- one device ------------------------------------------------------

// Returns 1 if the device ends up with an address, by lease or by
// claim.
// --- remembering a lease ----------------------------------------------
//
// So a REBOOT asks for the address it had. RFC 2131 calls it
// INIT-REBOOT: broadcast a REQUEST with ciaddr 0 and the remembered
// address in the requested-IP option, and a server that still holds
// that binding ACKs it. Without this every boot is a fresh DISCOVER,
// which is the other half of why this machine's address kept moving.
//
// KEYED TO THE DEVICE AND ITS MAC. An adapter swapped between boots
// gets a different binding, and asking for the previous card's address
// earns a NAK at best.

static void lease_path(char *out, uint32_t cap, const char *dev) {
    snprintf(out, cap, "%s/dhcp-%s.lease", LEASE_DIR, dev);
}

// Forgets a remembered address that has been refused. Without this a
// machine moved to another network spends the whole of WAIT_MS on a
// doomed INIT-REBOOT before every single retry, for the life of the
// boot.
static void forget_lease(const char *dev) {
    char path[LEASE_MAX];
    lease_path(path, sizeof path, dev);
    uconf_set(path, "ip", "");
}

static void save_lease(const char *dev, const uint8_t *mac,
                       const struct lease *l) {
    if (!l->ip || !l->seconds) return;
    char LEASE_FILE[LEASE_MAX];
    lease_path(LEASE_FILE, sizeof LEASE_FILE, dev);
    // The directory first: `open()` with O_CREAT does not create a
    // missing parent (POSIX), so without this a lease would never
    // persist -- loudly, as -ENOENT, since the parent check landed.
    // ASKED FIRST rather than created unconditionally: the kernel logs
    // `mkdir() rejected -- already exists`, and on a machine that
    // renews for weeks that is a line of noise per renewal in a ring
    // buffer holding a few hundred.
    struct sys_stat st;
    if (sys_stat(LEASE_DIR, &st) != 0) sys_mkdir(LEASE_DIR);

    char v[24];
    snprintf(v, sizeof v, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!uconf_set(LEASE_FILE, "mac", v)) return;
    uconf_set(LEASE_FILE, "device", dev);
    ip_str(v, l->ip);     uconf_set(LEASE_FILE, "ip", v);
    ip_str(v, l->server); uconf_set(LEASE_FILE, "server", v);
    snprintf(v, sizeof v, "%u", l->seconds);
    uconf_set(LEASE_FILE, "seconds", v);
}

// The remembered address for this card, or 0. Deliberately returns only
// the ADDRESS: everything else comes from the ACK, because a mask or a
// router remembered from a different network is worse than none.
static uint32_t remembered_ip(const char *dev, const uint8_t *mac) {
    char LEASE_FILE[LEASE_MAX];
    lease_path(LEASE_FILE, sizeof LEASE_FILE, dev);
    char want[24], got[24];
    snprintf(want, sizeof want, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!uconf_get(LEASE_FILE, "mac", got, sizeof got)) return 0;
    if (strcmp(got, want)) return 0;
    if (!uconf_get(LEASE_FILE, "device", got, sizeof got)) return 0;
    if (strcmp(got, dev)) return 0;
    if (!uconf_get(LEASE_FILE, "ip", got, sizeof got)) return 0;

    uint32_t ip = 0, part = 0;
    int n = 0, seen = 0;
    for (const char *c = got; ; c++) {
        // Bounded AS IT ACCUMULATES, not at the delimiter: uconf_get()
        // truncates into a fixed buffer, so a corrupt file can present a
        // digit run long enough to overflow -- and an overflow that -O2
        // is entitled to assume cannot happen takes the range check with
        // it.
        if (*c >= '0' && *c <= '9') {
            part = part * 10 + (uint32_t)(*c - '0');
            if (part > 255) return 0;
            seen = 1;
        }
        else if (*c == '.' || *c == '\0') {
            if (!seen || n > 3) return 0;
            ip = (ip << 8) | part;
            part = 0; seen = 0; n++;
            if (!*c) break;
        } else return 0;
    }
    return n == 4 ? ip : 0;
}

// --- sleeping longer than one syscall can ------------------------------
//
// `sys_sleep_ms()` is CLAMPED to SYS_SLEEP_MAX_MS (one hour) and says
// nothing about it, so a single call asking for T1 of a 24-hour lease
// returns after an hour and the caller believes its timer fired. That
// is what turned a twice-a-day renewal into an hourly one. Loop to a
// deadline instead of trusting one call.
static void sleep_seconds(uint32_t secs) {
    uint64_t deadline = sys_monotonic_ns() + (uint64_t)secs * 1000000000ull;
    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= deadline) return;
        uint64_t left_ms = (deadline - now) / 1000000ull;
        if (left_ms > SYS_SLEEP_MAX_MS) left_ms = SYS_SLEEP_MAX_MS;
        if (!left_ms) return;
        sys_sleep_ms((int)left_ms);
    }
}

// The FILE is Unix's name and the FORMAT is this repo's `key=value` --
// there is one config parser here and a second one for four bytes would
// be the drift nobody looks for. Shared, because a RENEWAL can move the
// nameserver and writing it in only one path meant it never did.
static void write_resolv(uint32_t dns) {
    char d[20];
    ip_str(d, dns);
    if (uconf_set(RESOLV_CONF, "nameserver", d))
        say("dhcp: nameserver %s -> %s\n", d, RESOLV_CONF);
    else
        say("dhcp: could not write %s\n", RESOLV_CONF);
}

// Puts a lease on the card and reports it. Shared by every path that
// obtains one -- INIT-REBOOT, DISCOVER/REQUEST -- so "what applying a
// lease means" has one definition rather than one per state.
static int apply(const struct query_netdev *dev, const struct lease *lp,
                 struct lease *got) {
    struct lease l = *lp;
    if (sys_net_config(dev->name, l.ip, l.mask, l.router) < 0) {
        cmd_fail("dhcp", dev->name);
        return 0;
    }

    char a[20], m[20], g[20];
    ip_str(a, l.ip); ip_str(m, l.mask); ip_str(g, l.router);
    say("dhcp: %s: %s netmask %s gateway %s\n", dev->name, a, m, g);

    if (l.dns) write_resolv(l.dns);
    if (l.seconds) say("dhcp: lease %u seconds\n", l.seconds);

    // Persisted HERE because this is the one place a lease becomes the
    // card's: doing it at each call site meant the boot path -- the
    // common one -- silently never wrote a lease at all.
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(dev->mac >> (i * 8));
    save_lease(dev->name, mac, &l);

    if (got) *got = l;
    return 1;
}

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
    struct lease l = {0};

    // INIT-REBOOT, when a lease was remembered: broadcast a REQUEST for
    // the address we had, ciaddr still 0 because we are not using it
    // yet. A server that still holds the binding ACKs it and the
    // machine keeps its address across a reboot; one that does not
    // either NAKs or stays quiet, and the DISCOVER below is the answer
    // to both. Skipping straight to DISCOVER is what made every boot
    // take whatever was free.
    uint32_t known = remembered_ip(dev->name, mac);
    if (known) {
        char a[20];
        ip_str(a, known);
        say("dhcp: %s: asking for %s again\n", dev->name, a);
        build(&out, mac, xid, MSG_REQUEST, known, 0, 0, &out_len);
        int n = exchange(fd, &out, out_len, xid, MSG_ACK, &in, BCAST);
        if (n > 0) {
            l.ip = get32(in.yiaddr);
            l.mask = option_ip(&in, (uint32_t)n, OPT_SUBNET_MASK);
            l.router = option_ip(&in, (uint32_t)n, OPT_ROUTER);
            l.dns = option_ip(&in, (uint32_t)n, OPT_DNS);
            l.server = option_ip(&in, (uint32_t)n, OPT_SERVER_ID);
            l.seconds = option_ip(&in, (uint32_t)n, OPT_LEASE_TIME);
            lease_timers(&l, &in, (uint32_t)n);
        }
        // THIS PATH MUST NOT BE ABLE TO PRODUCE A WORSE RESULT THAN THE
        // ONE BELOW IT. The DISCOVER path takes the mask from the OFFER
        // and only overrides it from the ACK, so it always has one;
        // there is no OFFER here, and an ACK that omits option 1 would
        // otherwise configure a 0.0.0.0 netmask with nothing on-link.
        // Anything short of a complete answer falls through and asks
        // properly.
        if (n > 0 && l.ip && l.mask) {
            close(fd);
            return apply(dev, &l, got);
        }
        if (n < 0) {
            // NAKed: the server knows this binding and has refused it,
            // so remembering it costs the full wait on every retry.
            say("dhcp: %s: %s refused, asking afresh\n", dev->name, a);
            forget_lease(dev->name);
        } else {
            say("dhcp: %s: no usable answer for %s, asking afresh\n",
                dev->name, a);
        }
        memset(&l, 0, sizeof l);

        // A FRESH TRANSACTION NEEDS A FRESH xid. The DISCOVER below is
        // not part of the exchange above, and a late ACK for the old one
        // arriving inside the new one's window would match on xid and
        // type and be taken as its reply. (The DISCOVER and its own
        // REQUEST DO share an xid -- those are one transaction.)
        if (sys_getrandom(xid, sizeof xid) != (int64_t)sizeof xid)
            put32(xid, (uint32_t)sys_monotonic_ns());
    }

    build(&out, mac, xid, MSG_DISCOVER, 0, 0, 0, &out_len);
    int len = exchange(fd, &out, out_len, xid, MSG_OFFER, &in, BCAST);
    if (len <= 0) {
        close(fd);
        say("dhcp: no offer on %s\n", dev->name);
        return link_local(dev->name, mac);
    }

    l.ip = get32(in.yiaddr);
    l.mask = option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK);
    l.router = option_ip(&in, (uint32_t)len, OPT_ROUTER);
    l.dns = option_ip(&in, (uint32_t)len, OPT_DNS);
    l.server = option_ip(&in, (uint32_t)len, OPT_SERVER_ID);

    // The REQUEST is what makes the offer a lease -- a client that
    // applied the offer without it is using an address the server still
    // considers free to hand to somebody else.
    build(&out, mac, xid, MSG_REQUEST, l.ip, l.server, 0, &out_len);
    len = exchange(fd, &out, out_len, xid, MSG_ACK, &in, BCAST);
    close(fd);
    if (len <= 0) {
        char a[20];
        ip_str(a, l.ip);
        say("dhcp: %s offered %s and did not acknowledge it\n", dev->name, a);
        return link_local(dev->name, mac);
    }
    // The ACK is authoritative, not the offer: a server may acknowledge
    // something other than what it offered.
    l.ip = get32(in.yiaddr);
    if (option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK)) l.mask = option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK);
    if (option_ip(&in, (uint32_t)len, OPT_ROUTER)) l.router = option_ip(&in, (uint32_t)len, OPT_ROUTER);
    if (option_ip(&in, (uint32_t)len, OPT_DNS)) l.dns = option_ip(&in, (uint32_t)len, OPT_DNS);
    l.seconds = option_ip(&in, (uint32_t)len, OPT_LEASE_TIME);
    lease_timers(&l, &in, (uint32_t)len);

    return apply(dev, &l, got);
}

// --- renewal ----------------------------------------------------------
//
// RFC 2131's timers: at T1 (half the lease) the client REQUESTs its own
// address from the server that granted it, and on an ACK the clock
// starts again. If that fails it keeps trying, and at expiry it falls
// all the way back to a DISCOVER -- because an address whose lease has
// run out is not ours any more, whatever the interface still says.
//
static struct lease k_lease;
static char renew_dev[NET_ABI_NAME_MAX];

// One RENEWING or REBINDING exchange: a REQUEST carrying ciaddr, to
// `dest`, expecting an ACK. Returns 1 and refreshes `l` on success.
//
// IT DOES NOT TOUCH THE INTERFACE ON FAILURE, which is the bug this
// replaces: the old path called configure(), and configure() assigns
// RFC 3927 link-local when nothing answers -- so one unanswered renewal
// threw away a lease with 22 hours left on it.
static int renew_once(const struct query_netdev *dev, const uint8_t *mac,
                      struct lease *l, uint32_t dest) {
    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) return 0;
    if (sys_bind(fd, 0, DHCP_CLIENT_PORT, dev->name) < 0) { close(fd); return 0; }

    uint8_t xid[4];
    if (sys_getrandom(xid, sizeof xid) != (int64_t)sizeof xid)
        put32(xid, (uint32_t)sys_monotonic_ns());

    static struct dhcp_msg out, in;
    uint32_t out_len = 0;
    // ciaddr set, no server-id, no requested-IP: RFC 2131's 4.3.6.
    build(&out, mac, xid, MSG_REQUEST, 0, 0, l->ip, &out_len);
    int len = exchange(fd, &out, out_len, xid, MSG_ACK, &in, dest);
    close(fd);
    if (len < 0) return -1;      // NAK: the binding is gone, stop asking
    if (!len) return 0;
    // An ACK naming no address is not one to apply: it would deconfigure
    // the card and leave every later REQUEST with ciaddr 0, which is a
    // shape no server grants.
    if (!get32(in.yiaddr)) return 0;

    // A server may move us; the ACK is authoritative either way.
    uint32_t was = l->ip, was_mask = l->mask, was_router = l->router;
    uint32_t was_dns = l->dns;
    l->ip = get32(in.yiaddr);
    if (option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK)) l->mask = option_ip(&in, (uint32_t)len, OPT_SUBNET_MASK);
    if (option_ip(&in, (uint32_t)len, OPT_ROUTER)) l->router = option_ip(&in, (uint32_t)len, OPT_ROUTER);
    if (option_ip(&in, (uint32_t)len, OPT_DNS)) l->dns = option_ip(&in, (uint32_t)len, OPT_DNS);
    if (option_ip(&in, (uint32_t)len, OPT_SERVER_ID)) l->server = option_ip(&in, (uint32_t)len, OPT_SERVER_ID);
    uint32_t secs = option_ip(&in, (uint32_t)len, OPT_LEASE_TIME);
    if (secs) l->seconds = secs;
    lease_timers(l, &in, (uint32_t)len);

    // APPLY WHATEVER MOVED, not only the address. A server is entitled
    // to keep the address and change the gateway or the mask, and
    // recording that in the struct without pushing it to the card left
    // the machine on the old values until it rebooted.
    if (l->ip != was) {
        char a[20], b[20];
        ip_str(a, was); ip_str(b, l->ip);
        say("dhcp: %s: moved from %s to %s\n", dev->name, a, b);
    }
    if (l->ip != was || l->mask != was_mask || l->router != was_router) {
        if (sys_net_config(dev->name, l->ip, l->mask, l->router) < 0)
            cmd_fail("dhcp", dev->name);
    }
    if (l->dns && l->dns != was_dns) write_resolv(l->dns);
    return 1;
}

// AS A SERVICE THIS NEVER RETURNS, and that is not a style choice: the
// descriptor says Restart=always, so a process that exits is restarted,
// and a restart that finds the card already addressed reports "nothing
// to do" and exits non-zero -- which init correctly calls a crash loop
// and gives up on. Measured on the laptop: five restarts in four
// seconds and then `dhcp is crash-looping, giving up`.
//
// So the loop owns both jobs. Without a lease it keeps ASKING, because
// a first attempt failing is the ordinary case rather than the end.
// With one it walks RFC 2131's states:
//
//     BOUND ---T1---> RENEWING ---T2---> REBINDING ---expiry---> INIT
//              unicast to the      broadcast to        give the
//              leasing server      anyone              address up
//
// EVERY DEADLINE IS MEASURED FROM THE ACK, not from the last attempt.
// A retry every 60s that also pushed the expiry back 60s would never
// expire, which is the failure mode that keeps an address the server
// has since given to somebody else.
//
// THE RETRY IS WHY THE BOOT LEASE WAS STILL BEING MISSED. Waiting for
// carrier was necessary and not sufficient: the link comes up at 4.43 s
// and the DISCOVER goes out immediately, but a switch port reports link
// before it forwards -- MAC learning, and spanning tree if it is
// enabled. The offer never arrives, and no length of carrier wait fixes
// that. Real clients retry, so this does.
#define RETRY_MIN_MS  3000
#define RETRY_MAX_MS 60000
#define RENEW_RETRY_MIN_S 60

static void supervise(void) {
    uint32_t backoff = RETRY_MIN_MS;
    // OUTSIDE the loop: re-deriving it per iteration meant a card that
    // briefly left QUERY_NETDEV restarted the lease clock, leaving every
    // deadline that much late.
    uint64_t acked = sys_monotonic_ns();
    for (;;) {
        struct query_netdev dev;
        int found = 0;
        for (unsigned i = 0; ; i++) {
            if (sys_query_record(QUERY_NETDEV, i, &dev, sizeof dev) < (int)sizeof dev) break;
            if (!strcmp(dev.name, renew_dev)) { found = 1; break; }
        }
        if (!found) { sleep_seconds(RETRY_MAX_MS / 1000); continue; }  // card gone

        uint8_t mac[6];
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(dev.mac >> (i * 8));

        if (!k_lease.seconds) {
            sys_sleep_ms((int)backoff);
            backoff = backoff * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : backoff * 2;
            struct lease got = {0};
            if (configure(&dev, &got) && got.seconds) {
                say("dhcp: %s: got a lease on retry\n", renew_dev);
                k_lease = got;      // apply() already persisted it
                acked = sys_monotonic_ns();
                backoff = RETRY_MIN_MS;
            }
            continue;
        }

        // Held. Every deadline is measured from the ACK that granted
        // this lease -- never from the last attempt, because a retry
        // that also pushed expiry back would never expire, and the
        // machine would keep an address the server has reassigned.
        //
        // WHY THE LOOP SAYS WHY IT ENDED. Deriving "it expired" from
        // having fallen out of the loop was wrong: a renewal that
        // SUCCEEDS with a shorter lease than the time already elapsed
        // then read as an expiry, and the recovery path can drop to
        // link-local -- throwing away the lease that was just renewed.
        // That is the same mistake as the `rebinding` flag before it:
        // a fact taken from control flow rather than stated.
        int expired = 0;

        for (;;) {
            uint32_t elapsed = (uint32_t)((sys_monotonic_ns() - acked) / 1000000000ull);
            if (elapsed >= k_lease.seconds) { expired = 1; break; }

            // When to act next: T1 while BOUND; otherwise half the time
            // remaining to the next milestone, floored at 60 s and never
            // past the milestone itself (RFC 2131 4.4.5). The clamp is
            // what makes REBINDING and expiry reachable at all.
            uint32_t target;
            if (elapsed < k_lease.t1) {
                target = k_lease.t1;
            } else {
                uint32_t milestone = elapsed < k_lease.t2 ? k_lease.t2 : k_lease.seconds;
                uint32_t remaining = milestone - elapsed;
                uint32_t wait = remaining / 2;
                if (wait < RENEW_RETRY_MIN_S) wait = RENEW_RETRY_MIN_S;
                if (wait > remaining) wait = remaining;
                target = elapsed + wait;
            }
            sleep_seconds(target - elapsed);

            elapsed = (uint32_t)((sys_monotonic_ns() - acked) / 1000000000ull);
            if (elapsed >= k_lease.seconds) { expired = 1; break; }

            // RENEWING goes to the server that granted it; REBINDING
            // asks anyone. The only difference on the wire is where it
            // is sent -- the message is the same shape.
            int rebinding = elapsed >= k_lease.t2;
            uint32_t dest = (!rebinding && k_lease.server) ? k_lease.server : BCAST;
            int rc = renew_once(&dev, mac, &k_lease, dest);
            if (rc > 0) {
                acked = sys_monotonic_ns();   // the NEW lease starts here
                say("dhcp: %s: renewed, lease %u seconds\n",
                    renew_dev, k_lease.seconds);
                save_lease(renew_dev, mac, &k_lease);
                break;                        // BOUND again, timers restart
            }
            if (rc < 0) {
                // NAKed: the server says this binding is gone, so there
                // is nothing to wait out.
                say("dhcp: %s: server refused the lease\n", renew_dev);
                forget_lease(renew_dev);
                expired = 1;
                break;
            }
            say("dhcp: %s: %s failed, retrying\n", renew_dev,
                rebinding ? "rebinding" : "renewal");
        }

        if (expired) {
            // The address is not ours any more, whatever the interface
            // still says -- the one place giving it up is right.
            say("dhcp: %s: lease expired, starting over\n", renew_dev);
            k_lease.seconds = 0;
        }
    }
}

int main(int argc, char **argv) {
    // ONE-SHOT BY DEFAULT, and staying resident is what has to be asked
    // for. The other way round was tried and is wrong: `dhcp net0` typed
    // at a prompt then never returned, because the supervisor below does
    // not exit. A command that hangs the terminal unless you know a flag
    // is a worse default than one that needs a flag to do the new thing.
    //
    // Deciding it from isatty() was the other candidate -- this file
    // already uses that to choose where diagnostics go -- and it was
    // rejected here: getting the LOG destination wrong is cosmetic,
    // getting this wrong hangs a prompt. The service descriptor says
    // `-k` and there is nothing to infer.
    int keep = 0;
    const char *want = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k")) keep = g_to_log = 1;
        else if (!strcmp(argv[i], "-1")) keep = 0;   // the default; accepted
        else if (!want && argv[i][0] != '-') want = argv[i];
        else { cmd_usage("dhcp [-k] [<device>]"); return 1; }
    }

    int tried = 0, done = 0;
    struct query_netdev dev;
    QUERY_FOREACH(QUERY_NETDEV, dev, i) {

        if (want) {
            if (strcmp(dev.name, want)) continue;
        } else if (dev.ip) {
            // Said out loud rather than skipped silently: on a boot
            // where one card is already configured this is the whole
            // difference between "nothing to do" and "nothing worked".
            say("dhcp: %s already has an address -- leaving it\n", dev.name);
            continue;
        }
        tried++;
        struct lease got = {0};
        if (configure(&dev, &got)) done++;
        // THE FIRST DEVICE TRIED IS THE ONE SUPERVISED, whether or not
        // it got a lease -- a card that fell back to link-local is
        // precisely the one worth asking again. One supervisor for one
        // card: a second would need its own timer, and no machine here
        // has ever held two leases at once.
        if (!renew_dev[0]) {
            k_lease = got.server ? got : (struct lease){0};
            strncpy(renew_dev, dev.name, sizeof renew_dev - 1);
        }
    }

    if (!tried) {
        if (want) say("dhcp: no such device: %s\n", want);
        else           say("dhcp: no device without an address\n");
        return 1;
    }
    // --- SUPERVISING (RFC 2131's renewal, plus a retry) ---------------
    //
    // Only when asked to: `-1` is the one-shot the boot used to be, and
    // is what a person typing `dhcp net0` at a prompt wants. As a
    // service this stays resident, because a lease that is never renewed
    // silently expires and the machine loses its address at an hour a
    // server chose.
    if (!keep || !renew_dev[0]) return done == tried ? 0 : 1;
    supervise();   // never returns
    return 0;
}
