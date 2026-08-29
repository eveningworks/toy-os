// ping -- the smallest end-to-end proof the network stack works.
//
// It exercises every layer in one command: ARP resolves the next hop,
// IPv4 routes and builds a header, ICMP carries the echo, the NIC
// driver puts it on the wire, and the whole path runs backwards for
// the reply. A `ping` that answers is worth more as evidence than any
// individual layer's test, which is why this is the first network
// program rather than a demo of one syscall.
//
// THE SOCKET DOES NOT BLOCK, so this polls. sys_recvfrom() returns 0
// for "nothing yet" (abi/syscall_abi.h says why blocking needs a wait
// channel per socket, which is a roadmap item), so a reply is waited
// for in short sleeps -- which is also what puts an upper bound on the
// round-trip time this reports.
//
// -EAGAIN FROM A SEND IS NOT A FAILURE. The first packet to a new
// destination usually gets it: the ARP request has just gone out and
// the reply has not come back. Retrying is the whole handling, and a
// ping that printed an error there would fail on every cold cache.
#include <stdint.h>
#include "rt/sys.h"
#include <stdio.h>
#include "lib/cmd.h"
#include "lib/uresolv.h"
#include <string.h>
#include <stdlib.h>

#define PAYLOAD_BYTES 56      // what every other ping sends
#define REPLY_WAIT_MS 1000
#define POLL_MS       10

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
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

int main(int argc, char **argv) {
    int count = 4;
    const char *target = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            count = atoi(argv[++i]);
            if (count < 1) count = 1;
        } else if (argv[i][0] == '-') {
            cmd_usage("ping [-c count] <address>");
            return 1;
        } else {
            target = argv[i];
        }
    }
    if (!target) { cmd_usage("ping [-c count] <address>"); return 1; }

    // A name is resolved through the same library `host` uses, so the
    // two cannot disagree about what a name means.
    uint32_t dst = 0;
    if (!parse_ip(target, &dst)) {
        int rc = uresolv_lookup(target, 0, &dst);
        if (rc < 0) {
            if (rc == -ENODEV)
                printf("ping: no nameserver configured -- run `dhcp`, or set one in %s\n",
                       URESOLV_CONF);
            else if (rc == -ENOENT)
                printf("ping: %s not found\n", target);
            else
                printf("ping: cannot resolve %s (%s)\n", target, strerror(-rc));
            return 1;
        }
        printf("ping: %s is %u.%u.%u.%u\n", target,
               (dst >> 24) & 0xFF, (dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF);
    }

    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_ICMP);
    if (fd < 0) {
        sys_print("ping: could not open a socket\n");
        return 1;
    }

    char line[128];
    snprintf(line, sizeof line, "PING %s: %d data bytes\n", target, PAYLOAD_BYTES);
    sys_print(line);

    uint8_t payload[PAYLOAD_BYTES];
    for (int i = 0; i < PAYLOAD_BYTES; i++) payload[i] = (uint8_t)('a' + (i % 26));

    int sent = 0, received = 0;
    for (int seq = 1; seq <= count; seq++) {
        // A send can report the ARP cache being cold. Retry over the
        // same budget the reply gets, so a cold cache costs latency
        // rather than a lost packet.
        int64_t rc = -1;
        for (int waited = 0; waited < REPLY_WAIT_MS; waited += POLL_MS) {
            rc = sys_sendto(fd, payload, sizeof payload, dst, 0);
            if (rc >= 0 || sys_errno() != EAGAIN) break;
            sys_sleep_ms(POLL_MS);
        }
        if (rc < 0) {
            // EAGAIN here has one meaning and it is not "try again": the
            // whole budget was spent retrying and nothing answered the
            // ARP request, so the host is not on this network. Saying
            // "try again" would send the reader back to the same command.
            if (sys_errno() == EAGAIN)
                snprintf(line, sizeof line, "ping: no ARP reply for %s -- is it on this subnet?\n",
                         target);
            else
                snprintf(line, sizeof line, "ping: send failed (%s)\n", strerror(sys_errno()));
            sys_print(line);
            break;
        }
        sent++;

        // ONE BLOCKING CALL, not a poll loop: the socket sleeps until
        // the reply arrives or the budget expires, so the time reported
        // is the round trip rather than a multiple of a poll interval.
        // Under an emulator that is often still 0.000 ms -- QEMU tends
        // to deliver the reply inside the sending syscall -- and that
        // number is now honest rather than an artefact of polling.
        uint64_t start = sys_monotonic_ns();
        uint8_t buf[PAYLOAD_BYTES + 16];
        uint32_t src = 0;
        int64_t n = sys_recvfrom(fd, buf, sizeof buf, &src, 0, REPLY_WAIT_MS);
        if (n > 0) {
            uint64_t us = (sys_monotonic_ns() - start) / 1000;
            snprintf(line, sizeof line,
                     "%lld bytes from %u.%u.%u.%u: icmp_seq=%d time=%llu.%03llu ms\n",
                     (long long)n, (src >> 24) & 0xFF, (src >> 16) & 0xFF,
                     (src >> 8) & 0xFF, src & 0xFF, seq,
                     (unsigned long long)(us / 1000), (unsigned long long)(us % 1000));
            sys_print(line);
            received++;
        } else {
            snprintf(line, sizeof line, "no reply from %s: icmp_seq=%d\n", target, seq);
            sys_print(line);
        }
        if (seq < count) sys_sleep_ms(200);
    }

    sys_close(fd);
    snprintf(line, sizeof line, "--- %s ping statistics ---\n"
                                "%d packets transmitted, %d received, %d%% packet loss\n",
             target, sent, received,
             sent ? (sent - received) * 100 / sent : 0);
    sys_print(line);

    // Exit status is the thing a test asserts on, so it says what
    // happened rather than whether the program ran: any reply is a
    // working stack, none is not.
    return received ? 0 : 1;
}
