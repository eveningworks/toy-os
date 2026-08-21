#ifndef ULIB_TUNABLE_H
#define ULIB_TUNABLE_H

// Reading and writing a kernel TUNABLE from ring 3 -- the write half of
// the commands whose read half is a query provider (`heap`, `ata`,
// `kstack`). A tunable is a setting that applies live and is not
// persisted; see docs/settings-and-queries.md's vocabulary.
//
// A header rather than three copies of the same twelve lines: /bin/heap,
// /bin/ata and /bin/kstack each flip exactly one, which is the bar
// (CLAUDE.md: a second real caller, not a plausible one).
//
// IT REPORTS THE REFUSAL, and that is the point of returning an int
// rather than void. `kernel.ata_nodma` can legitimately fail -- the
// driver will not switch modes with a non-blocking transfer in flight,
// because that would strand its poller -- so a caller that assumed
// success would print "PIO forced" while DMA carried on.
#include "rt/sys.h"
#include <string.h>

// Fills `out` with the tunable's current value. 1 on success, 0 if the
// registry has no such name.
static inline int tunable_get(const char *name, char *out, unsigned long cap) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, name, sizeof m.name);
    if (sys_setting(&m) != 0) return 0;
    strlcpy(out, m.value, cap);
    return 1;
}

// Applies `value`. Returns 1 if it took effect, 0 if the registry
// refused it -- an unknown name, a value the setting does not accept,
// or a subsystem that could not apply it right now.
static inline int tunable_set(const char *name, const char *value) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    strlcpy(m.name, name, sizeof m.name);
    strlcpy(m.value, value, sizeof m.value);
    if (sys_setting(&m) != 0) return 0;
    // SETTING_UNSAVED means "applied but will not survive a reboot",
    // which is true of every tunable BY DESIGN and is not a failure
    // here -- only SETTING_INVALID is.
    return m.result != SETTING_INVALID;
}

#endif // ULIB_TUNABLE_H
