#ifndef ULIB_UNISTD_H
#define ULIB_UNISTD_H

// The POSIX names for calls that already exist, and nothing else.
//
// EVERY FUNCTION HERE IS ONE LINE over rt/sys.h. The header earns its
// place because ported code writes `read(fd, buf, n)` and `#include
// <unistd.h>`, not because anything is implemented here -- and where a
// POSIX call has no toy-os equivalent it is ABSENT rather than stubbed.
// A fork() that always failed would be worse than a link error: the
// link error says "this program needs something this OS does not have",
// which is exactly true and exactly what a porter needs to read.
//
// So there is no fork/exec/pipe2/select/poll/sleep-with-signals here.
// SYS_SPAWN is posix_spawn-shaped rather than fork-shaped, deliberately
// (docs/init-design.md), and pretending otherwise in a header is how
// that decision would get quietly reversed.
#include <stddef.h>
#include <stdint.h>
#include "rt/sys.h"

typedef long ssize_t;
typedef long off_t;

static inline ssize_t read(int fd, void *buf, size_t n) {
    return (ssize_t)sys_read(fd, buf, n);
}
static inline ssize_t write(int fd, const void *buf, size_t n) {
    return (ssize_t)sys_write(fd, buf, n);
}
static inline int   close(int fd)                 { return sys_close(fd); }
static inline int   dup(int fd)                   { return sys_dup(fd); }
static inline int   dup2(int o, int n)            { return sys_dup2(o, n); }
static inline off_t lseek(int fd, off_t off, int whence) {
    return (off_t)sys_lseek(fd, off, whence);
}
static inline int   unlink(const char *p)         { return sys_unlink(p); }
static inline int   rmdir(const char *p)          { return sys_unlink(p); }
static inline int   chdir(const char *p)          { return sys_chdir(p); }
// sys_getcwd() returns the LENGTH, not 0 -- so the POSIX wrapper tests
// for the failure value rather than for success. Getting that backwards
// made getcwd() report failure on every call that worked, which is the
// shape of mistake a one-line wrapper is most prone to.
static inline char *getcwd(char *b, size_t n)     { return sys_getcwd(b, n) < 0 ? 0 : b; }
// A terminal is a FLAG of SYS_FSTAT rather than a call of its own --
// see abi/syscall_abi.h. Returns 1 or 0, as POSIX does, and 0 for a bad
// fd (POSIX also sets errno, which sys_fstat already did).
static inline int isatty(int fd) {
    struct sys_stat st;
    if (sys_fstat(fd, &st) != 0) return 0;
    return (st.flags & SYS_STAT_TTY) ? 1 : 0;
}

#endif
