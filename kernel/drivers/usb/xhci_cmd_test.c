// A KTEST for the command ring's abort-and-restart.
//
// SEPARATE FROM xhci_ring_test.c BECAUSE IT NEEDS A CONTROLLER. That
// file is pure arithmetic over a caller-supplied buffer and runs on a
// machine with no USB hardware; this one talks to real registers, so it
// SKIPS unless the kernel booted with an xHCI controller -- `make test`
// on the default QEMU line has none, and `tools/usb_test.py` (which
// boots `--usb xhci+mouse`) is where it actually runs.
//
// Why it exists: the fault it guards has only ever been seen on the
// bare-metal ASUS and never under QEMU, so the recovery would otherwise
// ship having never once executed -- to the machine whose network
// adapter is itself a USB device.
#include "ktest.h"
#include "xhci.h"

int xhci_selftest_cmd_recovery(void);

KTEST("xhci", "a timed-out command aborts the ring and leaves it usable") {
    int rc = xhci_selftest_cmd_recovery();
    if (rc < 0) KTEST_SKIP("no xHCI controller on this machine");
    // One assertion, deliberately: that the recovery FIRED and that a
    // command still completes afterwards. Split in two it would be
    // possible to pass the first and ship a ring nothing can use.
    KTEST_ASSERT(rc == 1);
}
