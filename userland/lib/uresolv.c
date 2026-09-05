// DNS, client side. See uresolv.h for what this deliberately is not.
//
// THE PARSER IS THE INTERESTING HALF, because a DNS reply is
// attacker-shaped data with a pointer format in it. A name in a message
// may end with a POINTER to an earlier offset (the top two bits of a
// length byte set), which exists so a message can repeat a domain
// cheaply -- and which lets a malicious or broken reply point a name at
// itself. Every walk here carries a JUMP BUDGET for that reason, and
// every read is bounded by the message length rather than by a
// terminator.
#include "uresolv.h"
#include "uconf.h"
#include "rt/sys.h"
#include "net_abi.h"
#include <string.h>

#define DNS_PORT     53
#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1
#define DNS_MAX      512     // the classic UDP message limit; no EDNS0 here
#define WAIT_MS      3000
#define POLL_MS      10

// A reply that made this many pointer jumps is looping or hostile.
// Sixteen is far more than any real message uses.
#define MAX_JUMPS 16

int uresolv_parse_ip(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!s) return 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned octet = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || octet > 255) return 0;
        }
        v = (v << 8) | octet;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

uint32_t uresolv_server(void) {
    char value[32];
    if (uconf_get(URESOLV_CONF, "nameserver", value, sizeof value) <= 0) return 0;
    uint32_t ip = 0;
    return uresolv_parse_ip(value, &ip) ? ip : 0;
}

// "www.example.com" -> \3www\7example\3com\0. Returns the bytes
// written, or 0 for a name that cannot be encoded: an empty label, a
// label over 63 bytes, or a name that would not fit.
static uint32_t encode_name(const char *name, uint8_t *out, uint32_t cap) {
    uint32_t written = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        uint32_t label = dot ? (uint32_t)(dot - p) : (uint32_t)strlen(p);
        if (label == 0 || label > 63) return 0;
        if (written + 1 + label + 1 > cap) return 0;
        out[written++] = (uint8_t)label;
        memcpy(out + written, p, label);
        written += label;
        p = dot ? dot + 1 : p + label;
    }
    if (written + 1 > cap) return 0;
    out[written++] = 0;
    return written;
}

// Step over a name, returning the offset just past it -- which for a
// name ending in a pointer is two bytes on from where the pointer was,
// NOT wherever it pointed. Returns 0 on anything malformed.
static uint32_t skip_name(const uint8_t *msg, uint32_t len, uint32_t off) {
    int jumps = 0;
    for (;;) {
        if (off >= len) return 0;
        uint8_t n = msg[off];
        if ((n & 0xC0) == 0xC0) {
            if (off + 2 > len) return 0;
            return off + 2;                 // a pointer always ends a name
        }
        if (n & 0xC0) return 0;             // reserved label type
        if (n == 0) return off + 1;
        off += 1u + n;
        if (++jumps > MAX_JUMPS) return 0;  // a chain this long is not real
    }
}

int uresolv_lookup(const char *name, uint32_t server, uint32_t *out_ip) {
    if (!name || !*name || !out_ip) return -EINVAL;
    if (!server) server = uresolv_server();
    if (!server) return -ENODEV;

    static uint8_t query[DNS_MAX], reply[DNS_MAX];

    // A random id, so a reply that is not ours is recognisable as such.
    // Not a security boundary -- there is no off-path attacker model
    // here -- but two lookups in flight need telling apart.
    uint16_t id = 0;
    if (sys_getrandom(&id, sizeof id) != (int64_t)sizeof id)
        id = (uint16_t)sys_monotonic_ns();

    query[0] = (uint8_t)(id >> 8); query[1] = (uint8_t)id;
    query[2] = 0x01; query[3] = 0x00;   // standard query, recursion desired
    query[4] = 0x00; query[5] = 0x01;   // one question
    query[6] = query[7] = query[8] = query[9] = query[10] = query[11] = 0;

    uint32_t n = encode_name(name, query + 12, DNS_MAX - 12 - 4);
    if (!n) return -EINVAL;
    uint32_t qlen = 12 + n;
    query[qlen++] = 0; query[qlen++] = DNS_TYPE_A;
    query[qlen++] = 0; query[qlen++] = DNS_CLASS_IN;

    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) return -ENODEV;

    int64_t rc = -1;
    for (int waited = 0; waited < WAIT_MS; waited += POLL_MS) {
        rc = sys_sendto(fd, query, qlen, server, DNS_PORT);
        if (rc >= 0 || sys_errno() != EAGAIN) break;   // ARP still resolving
        sys_sleep_ms(POLL_MS);
    }
    if (rc < 0) { sys_close(fd); return -EAGAIN; }

    // Blocking, on what is LEFT of the budget each time round: a reply
    // that is not ours must not extend the deadline.
    uint32_t len = 0;
    uint64_t deadline = sys_monotonic_ns() + (uint64_t)WAIT_MS * 1000000ull;
    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= deadline) break;
        unsigned left = (unsigned)((deadline - now) / 1000000ull);

        uint32_t src = 0;
        uint16_t port = 0;
        int64_t got = sys_recvfrom(fd, reply, sizeof reply, &src, &port, left ? left : 1);
        if (got <= 0) break;

        // Ours, and from the server we asked. A reply carrying somebody
        // else's id is not an error, it is somebody else's.
        if (got >= 12 && reply[0] == (uint8_t)(id >> 8) &&
            reply[1] == (uint8_t)id && src == server) {
            len = (uint32_t)got;
            break;
        }
    }
    sys_close(fd);
    if (!len) return -EAGAIN;

    uint8_t rcode = reply[3] & 0x0F;
    if (rcode == 3) return -ENOENT;   // NXDOMAIN: the name does not exist
    if (rcode != 0) return -EINVAL;

    uint32_t qdcount = ((uint32_t)reply[4] << 8) | reply[5];
    uint32_t ancount = ((uint32_t)reply[6] << 8) | reply[7];
    if (!ancount) return -ENOENT;     // answered, with nothing in it

    uint32_t off = 12;
    for (uint32_t i = 0; i < qdcount; i++) {
        off = skip_name(reply, len, off);
        if (!off || off + 4 > len) return -EINVAL;
        off += 4;                     // qtype and qclass
    }

    // EVERY answer is examined, not just the first: a name that is a
    // CNAME answers with the alias record AND the address record, in
    // that order, so a parser that reads only answer 0 fails on exactly
    // the names that are most common on the real internet.
    for (uint32_t i = 0; i < ancount; i++) {
        off = skip_name(reply, len, off);
        if (!off || off + 10 > len) return -EINVAL;
        uint32_t type = ((uint32_t)reply[off] << 8) | reply[off + 1];
        uint32_t cls  = ((uint32_t)reply[off + 2] << 8) | reply[off + 3];
        uint32_t rdlen = ((uint32_t)reply[off + 8] << 8) | reply[off + 9];
        off += 10;
        if (off + rdlen > len) return -EINVAL;
        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdlen == 4) {
            *out_ip = ((uint32_t)reply[off] << 24) | ((uint32_t)reply[off + 1] << 16) |
                      ((uint32_t)reply[off + 2] << 8) | (uint32_t)reply[off + 3];
            // Told to the kernel HERE rather than by each caller: this
            // is the one place a name and an address are both in hand,
            // and a caller that forgot would leave a nameless record in
            // somebody else's log. Its failure is ignored -- a lookup
            // that worked has not failed because a log did not want it.
            sys_net_resolved(name, *out_ip);
            return 0;
        }
        off += rdlen;
    }
    return -ENOENT;   // answers, but no address among them
}
