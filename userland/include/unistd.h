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
// So there is no pipe2/select/poll here, and sleep() is here but cannot
// be INTERRUPTED by a signal -- it returns 0 always, rather than POSIX's
// seconds-remaining, because there is no case in which that number
// could be anything else. fork() and exec*() ARE here (below), built
// for a ported shell; SYS_SPAWN and <spawn.h> stay the way a program
// written for toy-os starts another (docs/fork-design.md).
#include <stddef.h>
#include <stdint.h>
#include "rt/sys.h"

// THE THREE DESCRIPTORS EVERY PROCESS STARTS WITH. POSIX fixes the
// numbers and this kernel matches them; the names exist because ported
// code writes STDOUT_FILENO and would otherwise not compile over a
// difference that is not real.
//
// fd 2 is worth one clause: it always reaches the kernel log here, so a
// diagnostic is readable whoever spawned the process and wherever its
// stdout went -- which is why sys_eprint() is the channel a test tool
// asserts on. See docs/decisions.md, "stderr goes to the kernel log".
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

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
pid_t getppid(void);   // 0 for a process the kernel started

// fork() and exec*() exist for the program that cannot be written any
// other way -- a ported POSIX shell. Everything else should keep using
// sys_spawn()/posix_spawn(): one syscall, no address-space copy, and the
// child's streams and group named up front (docs/fork-design.md).
// A fork shares memory copy-on-write and copies the WHOLE descriptor
// table (there is no CLOEXEC); an exec keeps the descriptors, the cwd,
// the group and the parent, and resets caught signals.
static inline pid_t fork(void)                    { return sys_fork(); }
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);   // environ
int execvp(const char *file, char *const argv[]);  // PATH walk, then environ

static inline int pipe(int fds[2])                { return sys_pipe(fds); }
static inline int setpgid(pid_t pid, pid_t pgid)  { return sys_setpgid(pid, pgid); }
static inline pid_t getpgid(pid_t pid)            { return sys_getpgid(pid); }
static inline pid_t getpgrp(void)                 { return sys_getpgid(0); }
// A session is what a controlling terminal belongs to; setsid() starts
// one and drops the caller's. Refused for a process-group leader.
static inline pid_t setsid(void)                  { return sys_setsid(); }
static inline pid_t getsid(pid_t pid)             { return sys_getsid(pid); }

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

// --- who we are -------------------------------------------------------
//
// **ALL FIVE ANSWER 0 OR EMPTY, AND THAT IS A FACT RATHER THAN A STUB.**
// This system has no login, no /etc/passwd and no second user. Zero is
// root's id on every Unix, which is also the truthful answer here -- the
// one user can do everything -- so a program branching on
// `geteuid() == 0` takes the branch that is correct for this machine.
static inline uid_t getuid(void)  { return 0; }
static inline uid_t geteuid(void) { return 0; }
static inline gid_t getgid(void)  { return 0; }
static inline gid_t getegid(void) { return 0; }

// The supplementary group list, which is empty. Returns the COUNT, 0,
// for any size -- including the size-0 form that asks how many there
// are.
static inline int getgroups(int size, gid_t *list) { (void)size; (void)list; return 0; }

// **vfork() IS fork(), and the distinction it names does not exist
// here.** vfork promised a cheaper fork by sharing the parent's address
// space and suspending it until exec -- an optimisation for systems
// without copy-on-write. This fork IS copy-on-write
// (docs/conventions/kernel.md), so the promise is already kept, and the
// dangerous half of vfork's contract (a child that must touch nothing
// before exec) would buy nothing in exchange. POSIX removed it in 2008.
static inline pid_t vfork(void) { return fork(); }

// SEEK_SET/CUR/END are POSIX's here as well as <stdio.h>'s, since
// lseek() is declared in this header and code reaching for the constants
// includes whichever it saw first.
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

// --- sysconf ----------------------------------------------------------
//
// Runtime system values. Only the names below are answered; anything
// else is -1 with EINVAL, which is how POSIX says "this implementation
// has no limit for that" AND how it says "no such name" -- the two are
// distinguished by whether errno was set, so this sets it.
#define _SC_CLK_TCK      1
#define _SC_PAGESIZE     2
#define _SC_PAGE_SIZE    _SC_PAGESIZE
#define _SC_OPEN_MAX     3
#define _SC_ARG_MAX      4
#define _SC_NPROCESSORS_ONLN 5

// **_SC_CLK_TCK IS 1000000 HERE, NOT glibc's 100**, so that times()'s
// unit and clock()'s CLOCKS_PER_SEC are the same number and the one
// real trap in <sys/times.h> costs nothing. The kernel measures CPU
// time in nanoseconds (abi/proc_info.h), so a coarser tick would
// throw away precision it already has to buy compatibility with a
// constant no correct program hardcodes.
long sysconf(int name);

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
// BSD's scanner reset: set it to 1 to begin a fresh getopt() loop, which
// optind alone cannot do (it cannot say "and forget where you were
// inside a cluster like -la"). Cleared by getopt().
extern int optreset;
int getopt(int argc, char *const argv[], const char *optstring);

#endif
