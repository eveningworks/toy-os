// The two ioctl() requests this system can honour, over the typed
// syscalls that are its real interface. See <sys/ioctl.h>.
#include <sys/ioctl.h>
#include <termios.h>
#include <errno.h>
#include <stdarg.h>

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    struct winsize *ws = va_arg(ap, struct winsize *);
    va_end(ap);

    if (!ws) { errno = EINVAL; return -1; }
    switch (request) {
    case TIOCGWINSZ: return tcgetwinsize(fd, ws);
    case TIOCSWINSZ: return tcsetwinsize(fd, ws);
    default:
        // REFUSED, not quietly successful. A caller asking for something
        // this system does not do has to be able to find out.
        errno = EINVAL;
        return -1;
    }
}
