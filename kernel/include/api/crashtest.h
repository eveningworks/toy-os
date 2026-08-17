#ifndef API_CRASHTEST_H
#define API_CRASHTEST_H

#include <stdint.h>
#include "crash_abi.h"

// Deliberate kernel faults, for exercising the panic path. See
// crash_abi.h for the ring-3 view and kernel/core/crashtest.c for the
// triggers themselves.
//
// The KERNEL owns the list, so "what faults can this kernel produce?"
// has exactly one answer and every front end enumerates rather than
// hardcoding -- the same reason the settings registry exists. Adding a
// kind here gives the GUI app its button with no edit to the app.

struct crash_kind {
    const char *name; // short, stable: "gp-fault"
    const char *desc; // one line, shown in a UI
    void (*trigger)(void);
};

int crash_kind_count(void);
const struct crash_kind *crash_kind_at(int index);

// Whether the kernel will actually fault on request: 1 only when
// `faultinject` is on the GRUB command line. Off by default, because a
// deliberate crash hole has no business being open on an ordinary boot
// -- same switch pattern as `nokaslr`/`nopat`/`notsc`.
int crash_armed(void);

// Faults, on purpose. Returns 0 if the index is unknown or the kernel
// is not armed (and says which, in the log); otherwise it does not
// return at all, because the machine has panicked.
int crash_trigger(int index);

#endif
