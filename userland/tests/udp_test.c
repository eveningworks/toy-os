// UDP sockets, from ring 3.
//
// TWO MODES, because the two halves need different things. With NO
// ARGUMENTS it checks what needs no network at all -- binding, the
// ephemeral range, a port refused twice, what each protocol will and
// will not accept -- which is what lets it run in the ordinary gate
// where nothing is listening anywhere. With `<ip> <port> <text>` it
// sends a datagram and waits for one back, which is what
// tools/net_test.py drives with a real socket on the HOST at the other
// end: the only oracle that shares no code with this OS.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// A SPAWNED test reports through a file, not stdout: a spawned
// process's console output arrives while the harness is between
// commands, where it is dropped (tools/usertest_run.py). It is spawned
// rather than `run` because two of its checks are about BLOCKING, and
// the legacy loader has no scheduler slot to block on.
#define VERDICT_PATH "/tmp/udp_test.out"

#define REPLY_WAIT_MS 3000
#define POLL_MS       10

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) failures++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

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
    return *s ? 0 : (*out = v, 1);
}

static int local_checks(void) {
    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    check(fd >= 0, "a UDP socket opens");
    if (fd < 0) return 1;

    // An explicit port comes back as itself; that return value is what
    // makes the ephemeral case below readable at all.
    int port = sys_bind(fd, 0, 5353, 0);
    check(port == 5353, "bind to a named port returns that port");

    int other = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    check(sys_bind(other, 0, 5353, 0) < 0, "the same port is refused a second time");

    // Zero means "pick one", and it must land in the ephemeral range --
    // a kernel that returned 0 or reused 5353 would pass a check that
    // only asked whether bind succeeded.
    int eph = sys_bind(other, 0, 0, 0);
    check(eph >= NET_PORT_EPHEMERAL_LO && eph <= 65535,
          "bind to port 0 allocates from the ephemeral range");
    check(eph != 5353, "and not one already taken");
    sys_close(other);

    // Binding a device that does not exist must fail rather than bind
    // to nothing: a DHCP client that silently lost its interface would
    // broadcast out of whichever card happened to be first.
    int third = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    check(sys_bind(third, 0, 0, "net99") < 0, "binding an unknown device is refused");
    sys_close(third);

    // An ICMP socket has no port to bind, and says so rather than
    // accepting and ignoring.
    int icmp = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_ICMP);
    check(sys_bind(icmp, 0, 1234, 0) < 0, "an ICMP socket refuses bind");
    sys_close(icmp);

    check(sys_sendto(fd, "x", 1, 0x0A000202u, 0) < 0, "a send with no destination port fails");

    // A BOUNDED wait on a socket nothing will ever send to: it must
    // come back 0 after the timeout rather than -EAGAIN (a datagram
    // socket has no end-of-stream for a zero to be confused with) and
    // rather than never.
    uint32_t src = 0;
    uint16_t sport = 0;
    uint64_t before = sys_monotonic_ns();
    check(sys_recvfrom(fd, 0, 0, &src, &sport, 60) == 0,
          "a timed receive with nothing to receive returns 0");
    uint64_t waited_ms = (sys_monotonic_ns() - before) / 1000000ull;
    // It must actually WAIT. A kernel that returned 0 immediately would
    // pass the check above and turn every client's timeout into a spin.
    check(waited_ms >= 40, "and it really waited for the timeout");

    check(sys_set_nonblock(fd, 1) == 0, "a socket can be made non-blocking");
    check(sys_recvfrom(fd, 0, 0, &src, &sport, 0) == 0,
          "a non-blocking receive returns 0 without waiting");

    sys_close(fd);
    check(sys_recvfrom(fd, 0, 0, &src, &sport, 10) < 0, "a closed socket is a bad fd");

    // A DESCRIPTION MUST NOT INHERIT THE LAST ONE'S FLAGS. The fd table
    // is a pool, and `nonblock` used to survive a close: a program that
    // had set it handed the flag to the NEXT program's socket, whose
    // blocking receive then returned 0 at once and lost every reply.
    // It reproduced only after something unrelated had run, which is
    // why the check is here rather than left to be noticed.
    int fresh = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    uint64_t t0 = sys_monotonic_ns();
    sys_recvfrom(fresh, 0, 0, &src, &sport, 60);
    check((sys_monotonic_ns() - t0) / 1000000ull >= 40,
          "a reused descriptor does not inherit a non-blocking flag");
    sys_close(fresh);

    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        int rc = local_checks();
        char verdict[80];
        snprintf(verdict, sizeof verdict, "udp_test: %d checks, %d failed\n",
                 checks, failures);
        printf("%s", verdict);
        int fd = sys_open(VERDICT_PATH, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
        if (fd >= 0) {
            sys_write(fd, verdict, strlen(verdict));
            sys_close(fd);
        }
        return rc;
    }

    uint32_t dst = 0;
    if (!parse_ip(argv[1], &dst)) {
        printf("udp_test: not an address: %s\n", argv[1]);
        return 1;
    }
    int port = atoi(argv[2]);
    const char *text = argv[3];

    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) { printf("udp_test: no socket\n"); return 1; }

    int64_t rc = -1;
    for (int waited = 0; waited < REPLY_WAIT_MS; waited += POLL_MS) {
        rc = sys_sendto(fd, text, strlen(text), dst, (uint16_t)port);
        if (rc >= 0 || sys_errno() != EAGAIN) break;   // ARP still resolving
        sys_sleep_ms(POLL_MS);
    }
    if (rc < 0) {
        printf("udp_test: send failed (%s)\n", strerror(sys_errno()));
        return 1;
    }
    printf("udp_test: sent %lld bytes to %s:%d\n", (long long)rc, argv[1], port);

    char buf[512];
    uint32_t src = 0;
    uint16_t sport = 0;
    int64_t n = sys_recvfrom(fd, buf, sizeof buf - 1, &src, &sport, REPLY_WAIT_MS);
    if (n > 0) {
        buf[n] = 0;
        printf("udp_test: got %lld bytes from %u.%u.%u.%u:%u: %s\n",
               (long long)n, (src >> 24) & 0xFF, (src >> 16) & 0xFF,
               (src >> 8) & 0xFF, src & 0xFF, sport, buf);
        sys_close(fd);
        return 0;
    }
    printf("udp_test: no reply\n");
    sys_close(fd);
    return 1;
}
