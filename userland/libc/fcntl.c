// open(), the variadic POSIX form over the kernel's fixed-arity one.
#include <fcntl.h>
#include <stdarg.h>
#include "rt/sys.h"
#include <errno.h>

int open(const char *path, int flags, ...) {
    // The mode argument is READ AND DISCARDED rather than left on the
    // stack: a variadic function that never calls va_start is legal,
    // but doing it here documents that the argument is expected and
    // deliberately unused, instead of looking like an oversight.
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        (void)va_arg(ap, int);
        va_end(ap);
    }
    // **O_RDWR IS REFUSED, NOT QUIETLY DOWNGRADED.** An open here is a
    // reader or a writer, never both (fcntl.h). This used to alias
    // O_RDWR to O_WRONLY so ported code kept working, which held right
    // up against O_TRUNC: opening for update then truncated the file
    // before the first read failed. Refusing costs a caller an errno;
    // aliasing cost it the file.
    if ((flags & O_RDWR) == O_RDWR) {
        errno = EINVAL;
        return -1;
    }
    return sys_open(path, flags);
}


// fcntl -- see <fcntl.h> for why this is a function and ioctl() is not.
// Every command forwards to a typed syscall.
int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    switch (cmd) {
    case F_DUPFD:
        return sys_dupfd(fd, arg);
    case F_GETFD: {
        int r = sys_fd_cloexec(fd, -1);      // -1 queries
        return r < 0 ? -1 : (r ? FD_CLOEXEC : 0);
    }
    case F_SETFD: {
        // Only one flag exists, and anything else is refused rather
        // than silently dropped -- a caller setting a flag this system
        // does not have must not be told it worked.
        if (arg & ~FD_CLOEXEC) { errno = EINVAL; return -1; }
        int r = sys_fd_cloexec(fd, (arg & FD_CLOEXEC) ? 1 : 0);
        return r < 0 ? -1 : 0;
    }
    case F_GETFL: {
        // **O_NONBLOCK IS THE ONLY STATUS FLAG THIS SYSTEM TRACKS**, so
        // it is the only one that can be reported. The access mode is
        // deliberately NOT synthesised: an open here is a reader or a
        // writer and the kernel does not report which, so returning
        // O_RDONLY would be a guess -- and O_RDONLY is 0, which a
        // caller cannot distinguish from "no flags".
        struct sys_stat st;
        if (sys_fstat(fd, &st) < 0) return -1;
        return (st.flags & SYS_STAT_NONBLOCK) ? O_NONBLOCK : 0;
    }
    case F_SETFL:
        // Refuse anything but O_NONBLOCK: the rest are either fixed at
        // open time (O_APPEND) or absent, and accepting them would be
        // the silent ignore this library declines elsewhere.
        if (arg & ~O_NONBLOCK) { errno = EINVAL; return -1; }
        return sys_set_nonblock(fd, (arg & O_NONBLOCK) ? 1 : 0);
    default:
        errno = EINVAL;
        return -1;
    }
}
