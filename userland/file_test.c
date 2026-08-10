// A freestanding userland test program that exercises the new fd-aware
// file I/O syscalls (SYS_OPEN/SYS_READ/SYS_CLOSE, plus the now-fd-aware
// SYS_WRITE -- see syscall_abi.h) against the in-memory filesystem
// (fs.c). Opens a file for writing (creating it), writes a fixed
// string, closes it, reopens the SAME file for reading, reads it back,
// and writes what it read to stdout (fd 1) so the round-trip is visible
// on screen -- then exits 0 if the read-back bytes matched what was
// written, or 1 if anything about the round-trip didn't check out
// (open/read/write failed, or the bytes differ). This is the first
// program in this project to touch a real named file from ring 3 --
// every earlier test only ever wrote to the console.
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

static inline int64_t sys_open(const char *path, uint64_t flags) {
    return syscall2(SYS_OPEN, (uint64_t)(uintptr_t)path, flags);
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

#define READ_CAP 128

void _start(void) {
    const char *path = "filetest.txt";
    const char *content = "hello from a real file, round-tripped through fs.c\n";
    uint64_t content_len = my_strlen(content);

    // Write phase: open for writing, creating it if it's not there yet
    // and truncating any leftover content from a previous run.
    int64_t wfd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (wfd < 0) {
        const char *msg = "filetest: open(write) failed\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }
    int64_t wrote = sys_write((int)wfd, content, content_len);
    if (wrote != (int64_t)content_len) {
        const char *msg = "filetest: write() wrote the wrong number of bytes\n";
        sys_write(1, msg, my_strlen(msg));
        sys_close((int)wfd);
        sys_exit(1);
    }
    sys_close((int)wfd);

    // Read phase: reopen the same file for reading (default: read-only,
    // no flags needed) and read it back.
    int64_t rfd = sys_open(path, 0);
    if (rfd < 0) {
        const char *msg = "filetest: open(read) failed\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }
    char buf[READ_CAP];
    int64_t got = sys_read((int)rfd, buf, READ_CAP);
    sys_close((int)rfd);
    if (got < 0) {
        const char *msg = "filetest: read() failed\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }

    {
        const char *prefix = "filetest: read back: ";
        sys_write(1, prefix, my_strlen(prefix));
    }
    sys_write(1, buf, (uint64_t)got);

    // Verify the round trip actually matched byte-for-byte.
    int ok = (got == (int64_t)content_len);
    for (int64_t i = 0; ok && i < got; i++) {
        if (buf[i] != content[i]) ok = 0;
    }

    if (ok) {
        const char *msg = "filetest: round trip OK\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(0);
    } else {
        const char *msg = "filetest: round trip MISMATCH\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }
}
