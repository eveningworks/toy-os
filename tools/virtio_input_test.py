#!/usr/bin/env python3
"""tools/virtio_input_test.py -- virtio keyboard, mouse and tablet.

WHY IT EXISTS
-------------
Same reason as virtio_gpu_test.py: nothing else attaches these devices,
so without a tool that supplies the hardware the driver has no coverage
at all -- and the in-kernel `input` KTESTs deliberately cover the
VOCABULARY (input_report_*) rather than the device, because that half is
testable on any machine and this half is not.

WHAT IT ASSERTS, and the interesting ones are the last two:

  * all three devices are claimed with the right capabilities, and the
    tablet is the only one reporting `abs`;
  * they are IRQ-DRIVEN, and two of them SHARE a line -- which is the
    case kernel/arch/x86_64/irq.c's handler chain was rewritten for.
    The chipset routes PCI functions onto four wires, so this is not a
    contrived case, it is what happens;
  * an absolute position lands EXACTLY where the arithmetic says, which
    is what proves the tablet's range is being scaled to the screen
    rather than passed through;
  * THE GUEST SURVIVES POINTER MOTION DURING ENUMERATION, which is the
    check that exists because the opposite hung the machine. Motion is
    injected from the instant QEMU starts, so the tablet has events
    waiting the moment its INTx is enabled. Before the fix this hung
    3 boots out of 3, mid-log-line, with nothing else wrong; the device
    was made interrupt-capable before the handler could see it, so
    nobody read its ISR and a level-triggered line asserted forever.
    A boot that merely completes is the whole assertion -- there is
    nothing subtle to measure, because the failure is a dead machine;
  * a keypress reaches the DESKTOP -- Super opens the Start menu, which
    means the event crossed the driver, the input core, the key ring,
    the kernel's raw-input forwarder and the ring-3 compositor. An
    event counter alone would not have proved any of that.

The guest keeps its PS/2 pair as well, which is the point: the input
core is supposed to take several sources at once.

USAGE
    python3 tools/virtio_input_test.py [--instance N]
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole      # noqa: E402
from qmp_test import QMPSession         # noqa: E402
from harness import Results  # noqa: E402


class _Jiggler(threading.Thread):
    """Absolute pointer motion, injected over QMP as fast as it will go.

    It connects with retries because QEMU may not exist yet, and it
    swallows every error: the guest is expected to be booting, dying or
    absent underneath it, and none of that is this thread's business to
    report. The ASSERTION is whether the boot completed.
    """

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()
        self.join(timeout=2)

    def run(self):
        sock = None
        x = 0
        while not self._stop.is_set():
            if sock is None:
                try:
                    sock = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                    f = sock.makefile("rwb")
                    f.readline()
                    f.write(b'{"execute":"qmp_capabilities"}\n')
                    f.flush()
                    f.readline()
                except OSError:
                    sock = None
                    time.sleep(0.2)
                    continue
            x = (x + 4096) % 32000
            ev = {"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": x}},
                {"type": "abs", "data": {"axis": "y", "value": x}}]}}
            try:
                f.write((json.dumps(ev) + "\n").encode())
                f.flush()
                f.readline()
            except OSError:
                sock = None
            time.sleep(0.02)


Result = Results


def event_count(dbg):
    m = re.search(r"virtio-input: (\d+) event", dbg.send("lsdev"))
    return int(m.group(1)) if m else -1


def cursor_pos(dbg):
    m = re.search(r"cursor \((\d+),(\d+)\)", dbg.send("gui state"))
    return (int(m.group(1)), int(m.group(2))) if m else (-1, -1)


def screen_size(dbg):
    m = re.search(r"screen (\d+)x(\d+)", dbg.send("gui state"))
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def run(dbg, qmp, res, race_check=False, booted=True):
    dev = dbg.send("lsdev")

    # THE RACE CHECK, and it asks whether ENUMERATION FINISHED rather
    # than whether the boot "succeeded". A first version tested vm.py's
    # exit code and passed on a guest that had wedged partway through --
    # vm.py's readiness signal is the serial console, which comes up
    # BEFORE virtio-input runs, so a machine stuck in an interrupt storm
    # can still look ready. What cannot be faked is the device list: if
    # the third device storms, nothing after it is ever claimed.
    if race_check:
        res.check("all three devices enumerate with the pointer moving through boot",
                  booted and all(w in dev for w in ("QEMU Virtio Keyboard",
                                                     "QEMU Virtio Mouse",
                                                     "QEMU Virtio Tablet")),
                  "a hang or a short device list here is the virtio-input INTx "
                  "race (kernel/drivers/virtio/virtio_input.c): the device was "
                  "made interrupt-capable before its handler could see it")
    for want in ("QEMU Virtio Keyboard", "QEMU Virtio Mouse", "QEMU Virtio Tablet"):
        res.check(f'"{want}" was claimed', want in dev)
    res.check("the PS/2 pair is still registered beside them",
              "ps2-keyboard" in dev and "ps2-mouse" in dev)

    # Only the tablet reports absolute axes. If everything claimed `abs`
    # the capability would be telling us nothing.
    tablet_line = [l for l in dev.splitlines() if "Tablet" in l]
    mouse_line = [l for l in dev.splitlines() if "Virtio Mouse" in l]
    res.check("the tablet reports abs and the mouse does not",
              bool(tablet_line) and "abs" in tablet_line[0]
              and bool(mouse_line) and "abs" not in mouse_line[0],
              f"{tablet_line} / {mouse_line}")

    # --- interrupts, and the shared line ------------------------------
    #
    # Read from `lsdev`, NOT from dmesg. The boot line saying which IRQ
    # each device took has rolled out of the kernel's ring buffer by the
    # time a desktop has been up for a few seconds -- an oracle that
    # expires is an oracle that fails for reasons unrelated to the code,
    # so the servicing is a fact `lsdev` reports instead.
    irqs = re.findall(r"(QEMU Virtio [A-Za-z]+).*\[irq (\d+)\]", dev)
    res.check("every virtio input device is IRQ-driven", len(irqs) == 3,
              f"{irqs}" if irqs else "none reported an IRQ (are they polled?)")
    lines = [irq for _, irq in irqs]
    res.check("at least two devices SHARE an interrupt line "
              "(the case the handler chain exists for)",
              len(lines) > len(set(lines)), f"lines: {lines}")
    res.check("the PS/2 pair reports its own lines too",
              "[irq 1]" in dev and "[irq 12]" in dev)

    # A storm would have wedged the guest; answering at all is the proof,
    # so this check is worth stating rather than leaving implicit.
    res.check("the guest is still answering (no interrupt storm)",
              bool(dbg.send("gui state").strip()),
              "responded to a command after interrupts were enabled")

    # --- absolute positioning ------------------------------------------
    w, h = screen_size(dbg)
    res.check("the screen size was readable", w > 0 and h > 0, f"{w}x{h}")
    RANGE = 32767
    for frac_x, frac_y in ((0.5, 0.25), (0.0, 0.0), (1.0, 1.0)):
        ax, ay = int(RANGE * frac_x), int(RANGE * frac_y)
        qmp._cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]}})
        time.sleep(0.4)
        got = cursor_pos(dbg)
        want = (ax * (w - 1) // RANGE, ay * (h - 1) // RANGE)
        res.check(f"abs({ax},{ay}) puts the pointer at {want}", got == want, f"got {got}")

    # --- a key, all the way to the desktop -----------------------------
    before = event_count(dbg)
    qmp.send_key("a")
    time.sleep(0.4)
    res.check("keys are decoded by the driver", event_count(dbg) > before,
              f"{before} -> {event_count(dbg)}")

    # Super opens the Start menu (CLAUDE.md's GUI conventions), so this
    # single assertion covers the whole path from the virtqueue to the
    # ring-3 window manager.
    dbg.send("gui state")  # sync
    qmp.send_key("meta_l")
    time.sleep(0.6)
    state = dbg.send("gui state")
    opened = "start_menu=1" in state
    res.check("a virtio keypress reaches the ring-3 desktop (Super opens Start)",
              opened, [l for l in state.splitlines() if "start_menu" in l])
    if opened:
        qmp.send_key("esc")
        time.sleep(0.3)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    n = args.instance
    sock = ".vm.serial" if n == 0 else f".vm.{n}.serial"

    # KVM FOR THE BOOT, WHEN THERE IS ONE, BECAUSE THE RACE THIS CHECKS
    # FOR DOES NOT EXIST AT TCG SPEED. Measured: the buggy ordering hung
    # 3 boots out of 3 under KVM and 0 out of 3 under TCG, with the same
    # motion injected either way -- so a TCG-only run would report a
    # green check that cannot fail, which is worse than no check.
    kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
    launch = ["python3", "tools/vm.py", "--virtio-input",
              "--instance", str(n), "start"]
    if kvm:
        launch[3:3] = ["--kvm"]

    # THE POINTER MOVES WHILE THE KERNEL IS STILL ENUMERATING. Started
    # in the background so motion can be injected during the boot rather
    # than after it -- see the docstring. vm.py only touches QMP for
    # `shot`, so the port is ours while it waits on the serial console.
    proc = subprocess.Popen(launch)
    jiggler = _Jiggler(4445 + n)
    jiggler.start()
    rc = proc.wait()
    jiggler.stop()

    res = Result()
    if not kvm:
        print("  SKIP  the boot-with-motion check needs /dev/kvm "
              "(it cannot fail at TCG speed -- see the docstring)")
    dbg = None
    try:
        deadline = time.time() + 60
        while time.time() < deadline and dbg is None:
            try:
                dbg = DebugConsole(sock)
            except OSError:
                time.sleep(0.5)
        if dbg is None:
            print("virtio_input_test: no debug console (did the guest wedge?)")
            return 1
        while time.time() < deadline and "windows" not in dbg.send("gui state"):
            time.sleep(0.5)
        qmp = QMPSession(port=4445 + n)
        try:
            run(dbg, qmp, res, race_check=kvm, booted=(rc == 0))
        finally:
            qmp.close()
    finally:
        if dbg:
            dbg.close()
        if not args.keep:
            subprocess.run(["python3", "tools/vm.py", "--instance", str(n), "stop"],
                           stdout=subprocess.DEVNULL)

    print(f"\nvirtio_input_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
