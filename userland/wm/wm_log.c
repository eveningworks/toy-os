// See wm_log.h.
#include "wm/wm_log.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void wm_logf(const char *fmt, ...) {
    // 256 rather than something larger: the longest line the WM emits
    // today is a slow-frame report at ~90 characters, and a diagnostic
    // buffer on the stack competes with the four pages a ring-3 process
    // gets (kernel/uaddr.h).
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    // ONE CALL IS ONE LINE, printk's rule: a caller that left off the
    // "\n" ran the kernel log's next line onto its own
    // ("... in 6 ticksusb: port 5: connected").
    size_t n = strlen(buf);
    if (!n || buf[n - 1] != '\n') { buf[n] = '\n'; buf[n + 1] = 0; }
    sys_eprint(buf);
}
