// Toolkit diagnostics -- see ui/ulog.h.
#include "ui/ulog.h"
#include "rt/sys.h"     // sys_eprint
#include <stdio.h>      // vsnprintf
#include <stdarg.h>

void ulog(const char *s) {
    sys_eprint(s);
}

void ulogf(const char *fmt, ...) {
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sys_eprint(line);
}
