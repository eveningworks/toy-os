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
// So there is no fork/exec/pipe2/select/poll here, and sleep() is here
// but cannot be INTERRUPTED by a signal -- it returns 0 always, rather
// than POSIX's seconds-remaining, because there is no case in which
// that number could be anything else. SYS_SPAWN is posix_spawn-shaped
// rather than fork-shaped, deliberately (docs/init-design.md), and
// pretending otherwise in a header is how that decision would get
// quietly reversed.
#include <stddef.h>
#include <stdint.h>
#include "rt/sys.h"

// The POSIX type names live in <sys/types.h> now, guarded so that
// whichever header arrives first defines them -- code including only
// this one keeps working, which is what most of userland/ does.
#include <sys/types.h>

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
static inline int   fsync(int fd)                 { return sys_fsync(fd); }
// THE SAME CALL, and the duplication is honest rather than lazy.
// fdatasync(2) may skip metadata not needed to retrieve the data -- and
// here what a deferred write holds back IS the inode, so the metadata
// it is allowed to skip is exactly what has to land for the bytes to be
// findable. There is no cheaper subset to offer.
static inline int   fdatasync(int fd)             { return sys_fsync(fd); }
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


// --- processes --------------------------------------------------------

static inline pid_t getpid(void)                  { return sys_getpid(); }

// THERE IS NO fork(). SYS_SPAWN is posix_spawn-shaped, which is a
// decision with its own entry in docs/decisions.md rather than a gap --
// so there is no exec* family either, and nothing here pretends
// otherwise. Use sys_spawn()/sys_spawn_env() in "rt/sys.h".

static inline int pipe(int fds[2])                { return sys_pipe(fds); }
static inline int setpgid(pid_t pid, pid_t pgid)  { return sys_setpgid(pid, pgid); }
static inline pid_t getpgid(pid_t pid)            { return sys_getpgid(pid); }
static inline pid_t getpgrp(void)                 { return sys_getpgid(0); }

// The FOREGROUND process group of the terminal `fd` names -- what
// Ctrl-C interrupts. POSIX puts these in <unistd.h> rather than
// <termios.h>, and so do we.
static inline pid_t tcgetpgrp(int fd)             { return sys_tcgetpgrp(fd); }
static inline int   tcsetpgrp(int fd, pid_t pgid) { return sys_tcsetpgrp(fd, pgid); }

// Exit WITHOUT running atexit handlers or flushing streams -- which is
// what makes it different from exit(), and the reason a child that has
// inherited a parent's buffered output should call this one.
static inline void _exit(int code) { sys_exit(code); }

// --- does this path exist? --------------------------------------------
//
// **F_OK IS THE ONLY MODE THAT MEANS ANYTHING HERE.** access() asks
// whether a path is reachable AND whether the caller may read, write or
// execute it; this filesystem has no permission bits and no user to
// check them against (docs/filesystem-layout.md), so R_OK/W_OK/X_OK can
// only be answered as "yes, if it exists". They are accepted and treated
// as F_OK rather than refused, because a caller asking "can I read
// this?" on a system where every existing file is readable is asking a
// question with a correct answer.
//
// Returns 0 if the path exists, -1 with errno ENOENT if it does not.
int access(const char *path, int mode);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// --- sleeping ---------------------------------------------------------
//
// Both return 0 and never return early. POSIX's sleep() returns the
// seconds REMAINING when interrupted -- here nothing interrupts a
// sleep, so the answer is always 0 rather than a number a caller would
// have to check.
static inline unsigned sleep(unsigned sec)  { sys_sleep_ms((int)(sec * 1000)); return 0; }
static inline int usleep(unsigned long us)  { return sys_sleep_ms((int)(us / 1000)); }

// --- fd flags ---------------------------------------------------------

// Make a read that would block return -1/EAGAIN instead. This is
// fcntl(F_SETFL, O_NONBLOCK) on a real system; a named call here,
// because ioctl/fcntl's whole shape is a dispatch chain over an untyped
// argument and <fcntl.h> says why it is absent.
static inline int set_nonblock(int fd, int on) { return sys_set_nonblock(fd, on); }

// --- argument parsing -------------------------------------------------
//
// POSIX getopt. Parsing STOPS at the first non-option argument (BSD's
// and POSIX's behaviour), rather than permuting argv the way glibc
// does -- see userland/libc/getopt.c.
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

#endif
