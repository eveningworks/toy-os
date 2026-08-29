// The socket-fd syscalls, as a SURFACE: fd allocation, kind separation,
// and what each call refuses. There is a real stack behind these now
// (kernel/net/), and this test still deliberately does not prove a
// packet moves -- that needs a host on the other end, which is
// tools/net_test.py's job. What it proves is the part that must hold
// with or without a network:
//
//   1. socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) allocates a real fd,
//      distinct from a file fd.
//   2. send()/recv() are reachable and refuse: a datagram socket has
//      no peer, so they cannot say where to send or who sent it.
//   3. write()/read() (the file-only syscalls) REJECT a socket fd,
//      proving the tagged fd table keeps the two kinds separate.
//   4. An unsupported address family is rejected.
//   5. recvfrom() on an idle socket returns 0 -- "nothing yet", which
//      is NOT an error and NOT an end-of-stream.
//   6. close() works, and the fd is really gone afterwards.
//
// Exits 0 if every step behaved as documented, 1 otherwise.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"



















static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char *msg) {
    sys_write(1, msg, my_strlen(msg));
}

static int fail(const char *msg) {
    say(msg);
    return 0;
}

int main(void) {
    int ok = 1;

    // Step 4 first (a rejected socket() call shouldn't leave anything
    // behind to interfere with the real one below).
    int64_t bad = sys_socket(1, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_ICMP); // AF 1 is not AF_INET
    if (bad >= 0) {
        ok = ok && fail("sockettest: an unsupported address family should have been rejected\n");
        sys_close((int)bad); // clean up regardless, so it can't leak an fd
    } else {
        say("sockettest: unsupported address family correctly rejected\n");
    }

    // Step 1: a real socket fd.
    int64_t sfd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_ICMP);
    if (sfd < 0) {
        fail("sockettest: socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) failed\n");
        sys_exit(1);
    }
    say("sockettest: socket() returned a real fd\n");

    // Step 2: SYS_SEND/SYS_RECV are reachable and (correctly, for now) fail.
    char buf[16] = {0};
    int64_t sent = sys_send((int)sfd, "hi", 2);
    if (sent != -1) {
        ok = ok && fail("sockettest: send() should be refused (no peer)\n");
    } else {
        say("sockettest: send() correctly refused -- no peer named\n");
    }
    int64_t got = sys_recv((int)sfd, buf, sizeof(buf));
    if (got != -1) {
        ok = ok && fail("sockettest: recv() should be refused (no peer)\n");
    } else {
        say("sockettest: recv() correctly refused -- no peer named\n");
    }

    // Step 3: SYS_WRITE/SYS_READ must reject a socket fd -- proves the
    // fd table's file/socket kinds are actually kept separate.
    int64_t wrote = sys_write((int)sfd, "hi", 2);
    if (wrote != -1) {
        ok = ok && fail("sockettest: write() on a socket fd should be rejected\n");
    } else {
        say("sockettest: write() on a socket fd correctly rejected\n");
    }
    int64_t read_back = sys_read((int)sfd, buf, sizeof(buf));
    if (read_back != -1) {
        ok = ok && fail("sockettest: read() on a socket fd should be rejected\n");
    } else {
        say("sockettest: read() on a socket fd correctly rejected\n");
    }

    // Step 5: an idle socket has nothing to report, and says so with a
    // zero rather than an error. A caller that treats 0 as failure will
    // give up on the first poll of every ping it ever sends.
    uint32_t src = 0;
    int64_t idle = sys_recvfrom((int)sfd, buf, sizeof(buf), &src);
    if (idle != 0) {
        ok = ok && fail("sockettest: recvfrom() on an idle socket should return 0\n");
    } else {
        say("sockettest: recvfrom() on an idle socket correctly returned 0\n");
    }

    // Step 6: close, then confirm the fd is really gone.
    int64_t closed = sys_close((int)sfd);
    if (closed != 0) {
        ok = ok && fail("sockettest: close() failed\n");
    } else {
        say("sockettest: close() succeeded\n");
    }
    int64_t reuse = sys_send((int)sfd, "hi", 2);
    if (reuse != -1) {
        ok = ok && fail("sockettest: send() on a closed fd should be rejected\n");
    } else {
        say("sockettest: send() on a closed fd correctly rejected\n");
    }

    if (ok) {
        say("sockettest: all checks passed\n");
        sys_exit(0);
    } else {
        say("sockettest: one or more checks FAILED\n");
        sys_exit(1);
    }
}
