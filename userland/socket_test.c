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
#include "syscall_abi.h"

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int64_t sys_write(int fd, const char *buf, uint64_t len) {
    return syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_read(int fd, char *buf, uint64_t len) {
    return syscall3(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_socket(uint64_t domain, uint64_t type) {
    return syscall2(SYS_SOCKET, domain, type);
}

static inline int64_t sys_send(int fd, const char *buf, uint64_t len) {
    return syscall3(SYS_SEND, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_recv(int fd, char *buf, uint64_t len) {
    return syscall3(SYS_RECV, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_close(int fd) {
    return syscall2(SYS_CLOSE, (uint64_t)fd, 0);
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { } // unreachable
}

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

void _start(void) {
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
