#!/usr/bin/env python3
"""tools/serial_backpressure_test.py -- a stalled COM1 reader must not
stop the machine.

WHY THIS EXISTS
---------------
`serial_putc()` used to be `while (!transmit_empty()); outb(COM1, c);`
-- an unbounded wait on a bit the UART only sets when its consumer is
keeping up. QEMU's socket chardev stops setting it when an attached peer
stops draining, so the kernel spun there forever WITH INTERRUPTS ON:
ticks advanced, the PIC was clean, the scheduler kept picking the
compositor, and nothing ran. A klog burst inside an `FS_OP()` spun
holding the preemption guard, which is what made it total.

It reached `docs/bugs.md` as "a filesystem write during the desktop's
STARTUP stalls the clock", because the only tool that could see it was
`idle_desktop_test.py` -- the one tool that attaches a console, then
sits idle for ~3 s asserting that the screen MOVES. The filesystem write
was never the cause; it only supplied the log volume.

WHAT IT ASSERTS
---------------
With a client attached to the serial socket that NEVER READS, and the
guest asked to print something large, the taskbar clock must still tick.
That is the whole property: the machine keeps running while its log has
nowhere to go.

The clock is the probe because it is the one thing on an idle desktop
that changes on its own -- the same reason `idle_desktop_test.py` uses
it as its motion control.

POSITIVE CONTROL, verified: in `kernel/core/serial.c`, replace
`tx_putc()`'s body -- the one place both ports transmit -- with the
original `while (!transmit_empty(u)); outb(u->base, c);` and rebuild.
Measured 2026-09-26 with the log on COM1 and the console on COM2: 1
distinct clock image in 8, 2 runs of 2, against 4-5 without it. (Before
the split it was `serial_putc()`'s body, and one `sh dmesg` was load
enough; see DMESG_DUMPS for why it is not now.)

DO NOT "FIX" A FAILURE HERE BY DRAINING THE SOCKET. The whole point is
that the guest must survive a consumer that stopped reading; a drainer
in the harness makes this pass against a kernel that still hangs.

    python3 tools/vm.py start
    python3 tools/serial_backpressure_test.py
    echo $?
"""

import argparse
import hashlib
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_test import QMPSession  # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"

# Eight samples a third of a second apart -- the same cadence
# idle_desktop_test.py uses, so a failure here and a failure there are
# talking about the same window.
SAMPLES = 8
INTERVAL_S = 0.35
DMESG_DUMPS = 6   # see the send below for why one is not enough
AML_DUMPS = 20
FILL_S = 1.5


class Results:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, name, ok, detail=""):
        tail = f"    [{detail}]" if detail else ""
        print(f"  {'PASS' if ok else 'FAIL'}  {name}{tail}")
        if ok:
            self.passed += 1
        else:
            self.failed.append(name)


def clock_box(size):
    w, h = size
    return (max(0, w - 220), max(0, h - 30), w, h)


def main():
    ap = argparse.ArgumentParser()
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    args = ap.parse_args()
    port_guard.resolve_instance(args, "serial_backpressure_test")

    from PIL import Image

    res = Results()
    print("serial backpressure (a stalled COM1 reader must not stop the machine)")

    # Attach and go deliberately deaf. `connect` alone is what creates
    # the backpressure: with NO peer the chardev discards and the guest
    # never blocks, so an unattached run cannot see this bug at all.
    #
    # BOTH PORTS, when the guest has two: the kernel log is on COM1
    # (`.vm.N.log`) and the console on COM2, and a stall on COM1 -- a
    # klog burst inside an FS_OP() -- is the hang this exists for. Deaf
    # on the console alone, the log would go to a chardev with no peer
    # and be discarded, and the positive control below would stay green.
    deaf = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deaf.connect(args.sock)
    deaf_log = None
    log_path = port_guard.log_sock_for(args.sock)
    if log_path and os.path.exists(log_path):
        deaf_log = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deaf_log.connect(log_path)
    try:
        # Ask for something big enough to outrun the UART AND the host
        # socket's buffer, and never read the reply. `dmesg` is the whole
        # ring on the console port; `aml` is ~11 KB THROUGH THE KERNEL LOG,
        # so COM1 backs up too. One of each was absorbed by the socket
        # buffers once the log had its own port, and the positive control
        # stayed green -- so it is several, and the samples wait for them
        # to fill.
        deaf.sendall(b"sh dmesg\n" * DMESG_DUMPS + b"aml\n" * AML_DUMPS)
        time.sleep(FILL_S)

        qmp = QMPSession(port=args.qmp_port)
        tmp = os.environ.get("TMPDIR", "/tmp")
        seen = []
        for _ in range(SAMPLES):
            path = qmp.screenshot(os.path.join(tmp, "serial_bp.png"), settle=0)
            im = Image.open(path).convert("RGB")
            crop = im.crop(clock_box(im.size))
            seen.append(hashlib.md5(crop.tobytes()).hexdigest()[:10])
            time.sleep(INTERVAL_S)

        distinct = len(set(seen))
        res.check("the taskbar clock keeps ticking while COM1 backs up",
                  distinct > 1, f"{distinct} distinct in {SAMPLES}")
        qmp.close()
    finally:
        deaf.close()
        if deaf_log is not None:
            deaf_log.close()

    # And the guest is still there afterwards -- a kernel that merely
    # survived the window but left the console wedged would pass the
    # check above on a lucky sample.
    time.sleep(0.5)
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    probe.settimeout(6.0)
    try:
        probe.connect(args.sock)
        probe.sendall(b"\n")
        buf = b""
        deadline = time.time() + 6.0
        while time.time() < deadline and b"dbg> " not in buf:
            try:
                chunk = probe.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
        res.check("the console still answers once the reader comes back",
                  b"dbg> " in buf, f"{len(buf)} bytes")
    finally:
        probe.close()

    print(f"\nserial_backpressure_test: {res.passed} passed, "
          f"{len(res.failed)} failed")
    for name in res.failed:
        print(f"  FAILED: {name}")
    return 1 if res.failed else 0


if __name__ == "__main__":
    sys.exit(main())
