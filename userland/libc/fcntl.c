// open(), the variadic POSIX form over the kernel's fixed-arity one.
#include <fcntl.h>
#include <stdarg.h>
#include "rt/sys.h"

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
    return sys_open(path, flags);
}
