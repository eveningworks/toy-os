#include "debugflags.h"
#include "string.h"
#include <stdint.h>

const char *const DBGFLAG_NAMES[DBGFLAG_SUBSYS_COUNT] = {
    "fs",
    "wm",
    "ata",
};

static uint32_t g_dbgflag_mask = 0; // bit per dbgflag_subsys -- all off by default

int dbgflag_enabled(enum dbgflag_subsys s) {
    if ((unsigned)s >= DBGFLAG_SUBSYS_COUNT) return 0;
    return (g_dbgflag_mask >> (unsigned)s) & 1u;
}

void dbgflag_set(enum dbgflag_subsys s, int on) {
    if ((unsigned)s >= DBGFLAG_SUBSYS_COUNT) return;
    if (on) g_dbgflag_mask |= (1u << (unsigned)s);
    else g_dbgflag_mask &= ~(1u << (unsigned)s);
}

int dbgflag_parse(const char *name, enum dbgflag_subsys *out) {
    if (!name || !*name) return 0;
    for (int i = 0; i < DBGFLAG_SUBSYS_COUNT; i++) {
        if (k_strcmp(name, DBGFLAG_NAMES[i]) == 0) {
            *out = (enum dbgflag_subsys)i;
            return 1;
        }
    }
    return 0;
}
