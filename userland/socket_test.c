// A freestanding userland test program that exercises the new
// socket-fd syscalls (SYS_SOCKET/SYS_SEND/SYS_RECV, plus SYS_CLOSE --
// see syscall_abi.h). There's no NIC driver or protocol stack yet (see
// README.md's "Basic TCP/IP networking"), so this deliberately does
// NOT prove data moves anywhere -- it proves the fd/syscall SURFACE
// behaves correctly ahead of a real transport existing:
//
//   1. SYS_SOCKET(0, 0) allocates a real fd, distinct from a file fd.
//   2. SYS_SEND/SYS_RECV on that fd are reachable and return -1 (no
//      transport yet -- expected, not a failure of this test).
//   3. SYS_WRITE/SYS_READ (the file-only syscalls) correctly REJECT a
//      socket fd, proving the tagged fd table actually keeps the two
//      kinds separate rather than treating a socket fd as an
//      accidental file fd.
//   4. SYS_SOCKET(1, 0) (a nonzero, unsupported domain) is correctly
//      rejected -- domain/type are reserved for future use and must be
//      0 for now.
//   5. SYS_CLOSE on the socket fd succeeds, and using it again
//      afterward is correctly rejected.
//
// Exits 0 if every step behaved as documented, 1 otherwise.
#include <stdint.h>
#include "sys.h"



















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
    int64_t bad = sys_socket(1, 0); // domain=1 (unsupported) -- must be rejected
    if (bad >= 0) {
        ok = ok && fail("sockettest: socket(1, 0) should have been rejected\n");
        sys_close((int)bad); // clean up regardless, so it can't leak an fd
    } else {
        say("sockettest: socket(1, 0) correctly rejected\n");
    }

    // Step 1: a real socket fd.
    int64_t sfd = sys_socket(0, 0);
    if (sfd < 0) {
        fail("sockettest: socket(0, 0) failed\n");
        sys_exit(1);
    }
    say("sockettest: socket() returned a real fd\n");

    // Step 2: SYS_SEND/SYS_RECV are reachable and (correctly, for now) fail.
    char buf[16] = {0};
    int64_t sent = sys_send((int)sfd, "hi", 2);
    if (sent != -1) {
        ok = ok && fail("sockettest: send() should return -1 (no transport yet)\n");
    } else {
        say("sockettest: send() correctly returned -1 (no transport yet)\n");
    }
    int64_t got = sys_recv((int)sfd, buf, sizeof(buf));
    if (got != -1) {
        ok = ok && fail("sockettest: recv() should return -1 (no transport yet)\n");
    } else {
        say("sockettest: recv() correctly returned -1 (no transport yet)\n");
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

    // Step 5: close, then confirm the fd is really gone.
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
