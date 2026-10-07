#!/usr/bin/env python3
"""tools/msi_test.py -- the Local APIC, and the xHCI delivering through
MSI-X instead of a shared pin.

WHAT IS UNDER TEST
------------------
`lapic_init()` enables the Local APIC in virtual wire mode (so the 8259
keeps delivering every legacy line), and the xHCI asks for one of its
vectors: MSI-X first, MSI second, its INTx pin only if neither is there.

THE CHECK THAT MATTERS IS THAT INTERRUPTS ARRIVE, and it is not the
obvious one. Enumeration cannot show it: every control transfer in this
driver POLLS the event ring (xhci_xfer.c's wait_completion), so a controller
whose interrupts go nowhere still finds its devices, registers them and
logs "running" -- the whole boot looks perfect. What only an interrupt
can do is deliver a HID report asynchronously, so this moves the mouse
and requires BOTH the controller's interrupt count and its decoded-report
count to rise. With INTx disabled by the MSI-X programming, an interrupt
that arrives can only have come from the vector.

PHASE 2 IS VIRTIO, and it is the busier case: virtio-input and
virtio-net take a vector each through virtio_msix_enable(), which also
has to write the config-change and per-queue MSI-X table entries. The
same "it enumerates fine while nothing is delivered" hazard applies, so
the load-bearing check is again that a COUNT rises -- decoded input
events, and an ICMP round trip through the receive queue.

WHAT A CONTROL HERE MEASURED, because it is worth not re-deriving:
making virtio_irq_is_ours() gate on the ISR byte under MSI-X changed no
count at all. QEMU's virtio_irq() writes the ISR before dispatching the
vector, so the pre-MSI-X handler shape keeps working in emulation. The
control that DOES go red is returning 0 from it outright (0 events
decoded against 33 vectors delivered), which is what proves this tool
can see a dead path.

THE CONTROL IS A BOOT FLAG, and it is worth running by hand at least
once when this code changes:

    make iso KCMDLINE="nomsi" && python3 tools/vm.py --usb xhci+mouse start
    python3 tools/vm.py exec "lsdev"      # -> usb-xhci [irq 11], LAPIC not enabled
    make iso                              # put the plain media back

That path must still work: it is what a CPU with no APIC, or a machine
where MSI turns out to be broken, falls back to. Measured on 2026-08-30:
with `nomsi` the controller is back on IRQ 11 and mouse motion still
decodes reports, so the fallback is not theoretical.

    python3 tools/msi_test.py [--instance N]

On demand, not in the gate: it boots its own guest with USB hardware.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import port_guard  # noqa: E402
from gui_debug import DebugConsole  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from harness import copy_disk  # noqa: E402

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'ok   ' if ok else 'FAIL '} {name}" + (f"  -- {detail}" if detail else ""))
    return bool(ok)


def reports_and_irqs(dbg):
    """(irqs, reports) from the `usb` dump -- the controller's own
    counters, which is the only place either number exists."""
    u = dbg.send("usb") or ""
    irqs = re.search(r"(\d+) irq\(s\)", u)
    reps = re.search(r"(\d+) report\(s\) decoded", u)
    return (int(irqs.group(1)) if irqs else -1,
            int(reps.group(1)) if reps else -1)


def decoded_events(dbg):
    """virtio-input's own decode counter, out of `lsdev`."""
    m = re.search(r"virtio-input: (\d+) event\(s\) decoded", dbg.send("lsdev") or "")
    return int(m.group(1)) if m else -1


def virtio_phase(n, img):
    """virtio-input and virtio-net on vectors of their own."""
    print("\nmsi_test: virtio-input and virtio-net on MSI-X")
    if n is None:
        check("a free VM slot for the virtio phase", False)
        return
    sock = port_guard.instance_sock(n)
    boot = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                           "--instance", str(n), "--disk", img,
                           "--virtio-input", "--net", "virtio", "start"], cwd=REPO)
    if not check("the guest boots with virtio input and networking",
                 boot.returncode == 0):
        return
    try:
        time.sleep(3)
        dbg = DebugConsole(sock)
        qmp = QMPSession(port=4445 + n)

        lsdev = dbg.send("lsdev") or ""
        check("every virtio-input device took a vector, none a line",
              "[msi " in lsdev and "Virtio" in lsdev
              and not re.search(r"Virtio.*\[irq ", lsdev),
              "; ".join(ln.strip() for ln in lsdev.splitlines() if "Virtio" in ln))

        # THE LOAD-BEARING CHECK, same shape as the xHCI's: enumeration
        # proves nothing, a rising count does.
        ev0 = decoded_events(dbg)
        for _ in range(12):
            qmp.move_rel(9, 6)
            time.sleep(0.05)
        qmp.send_text("abc")
        time.sleep(1.0)
        ev1 = decoded_events(dbg)
        check("input on a vector reaches the input core",
              ev1 > ev0, f"{ev0} -> {ev1} events decoded")

        # virtio-net's receive queue is the other half: an ICMP reply
        # only arrives through the queue the vector notifies. 10.0.2.2
        # is QEMU's own SLIRP, so nothing leaves this machine.
        out = dbg.send("sh ping -c 3 10.0.2.2") or ""
        check("virtio-net receives on its vector",
              "0% packet loss" in out,
              next((ln.strip() for ln in out.splitlines() if "packet loss" in ln),
                   "no ping summary"))
        dbg.close()
    finally:
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(n), "stop"], capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", default="auto",
                    help="VM slot, or `auto` to take the lowest free one")
    args = ap.parse_args()
    n = (port_guard.find_free_instance() if args.instance == "auto"
         else int(args.instance))
    if n is None:
        print("msi_test: no free VM slot")
        return 2
    sock = os.path.join(REPO, ".vm.serial" if n == 0 else f".vm.{n}.serial")

    img = os.path.join(tempfile.gettempdir(), f"msi_test_{n}.img")
    copy_disk("disk.img", img, cwd=REPO)
    subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                    "--instance", str(n), "stop"], capture_output=True)
    boot = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                           "--instance", str(n), "--disk", img,
                           "--usb", "xhci+mouse", "start"], cwd=REPO)
    print("msi_test: the LAPIC, and the xHCI on a vector")
    if not check("the guest boots with an xHCI and a USB mouse",
                 boot.returncode == 0):
        return 1

    try:
        time.sleep(3)
        dbg = DebugConsole(sock)
        qmp = QMPSession(port=4445 + n)

        dmesg = dbg.send("sh dmesg") or ""
        check("the Local APIC came up",
              "lapic: id" in dmesg,
              next((ln.strip() for ln in dmesg.splitlines() if "lapic:" in ln),
                   "no lapic line"))
        # Virtual wire mode is what keeps the 8259 delivering; a machine
        # that got it wrong does not report anything, it stops at the
        # first timer tick. Reaching this line at all is the evidence.
        check("...and the machine still runs, so the 8259 still delivers",
              "toy-os: boot target" in dmesg or "init started" in dmesg)

        msi_line = next((ln.strip() for ln in dmesg.splitlines()
                         if "msix:" in ln or "msi:" in ln), "")
        check("the controller took a message-signalled vector", bool(msi_line),
              msi_line or "no msi/msix line -- it stayed on its pin")

        lsdev = dbg.send("lsdev") or ""
        check("lsdev reports the LAPIC and its claimed vectors",
              "LAPIC: id" in lsdev and "MSI vector(s) claimed" in lsdev,
              next((ln.strip() for ln in lsdev.splitlines() if "LAPIC" in ln), ""))
        check("...and the controller as MSI-X rather than a line",
              "running (MSI-X)" in lsdev or "running (MSI)" in lsdev,
              next((ln.strip() for ln in lsdev.splitlines() if "USB:" in ln), ""))
        # The HID devices mirror whichever way the controller is
        # serviced. Reading `irq` alone made them report themselves
        # polled on an MSI controller -- and install a poll thunk they
        # did not need.
        check("...and the HID devices as delivered on that vector",
              "usb-mouse" in lsdev and "[msi " in lsdev,
              next((ln.strip() for ln in lsdev.splitlines() if "usb-mouse" in ln), ""))

        # --- THE LOAD-BEARING CHECK ---------------------------------
        irqs0, reps0 = reports_and_irqs(dbg)
        for _ in range(15):
            qmp.move_rel(7, 5)
            time.sleep(0.06)
        time.sleep(1.0)
        irqs1, reps1 = reports_and_irqs(dbg)
        check("moving the mouse DELIVERS interrupts on the vector",
              irqs1 > irqs0, f"{irqs0} -> {irqs1} interrupts")
        check("...and the reports reach the input core",
              reps1 > reps0, f"{reps0} -> {reps1} reports decoded")

        # A handful of spurious interrupts over a boot would be normal;
        # a stream of them is a real symptom, and the counter is the
        # only place it would show.
        spur = re.search(r"(\d+) spurious", lsdev)
        check("no spurious interrupts", spur is not None and int(spur.group(1)) == 0,
              spur.group(0) if spur else "not reported")
        dbg.close()
    finally:
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(n), "stop"], capture_output=True)

    # A FRESH SLOT, not this one: QMPSession leaves the port in
    # TIME_WAIT for about a minute after phase 1, so reusing the slot
    # either waits that out or is refused as a clash with the guest
    # just killed (kbd_test.py hit the same thing).
    virtio_phase(port_guard.find_free_instance(), img)

    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nmsi_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
