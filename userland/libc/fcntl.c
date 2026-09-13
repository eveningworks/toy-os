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
