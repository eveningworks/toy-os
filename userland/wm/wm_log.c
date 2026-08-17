// See wm_log.h.
#include "wm/wm_log.h"
#include "rt/sys.h"
#include "lib/stdio.h"
#include <stdarg.h>

void wm_logf(const char *fmt, ...) {
    // 256 rather than something larger: the longest line the WM emits
    // today is a slow-frame report at ~90 characters, and a diagnostic
    // buffer on the stack competes with the four pages a ring-3 process
    // gets (kernel/uaddr.h).
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sys_eprint(buf);
}
