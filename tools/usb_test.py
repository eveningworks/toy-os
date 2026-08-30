#!/usr/bin/env python3
"""USB: an xHCI controller, and a HID boot keyboard and mouse.

WHY THIS TOOL IS SELF-CONTROLLING, which is the thing worth knowing
before changing it. QEMU activates a keyboard handler the moment
`usb-kbd` is attached and routes host keystrokes to THAT device, so a
guest whose USB driver does not work receives nothing at all -- not from
USB, and not from the PS/2 keyboard either. Measured before any driver
code was written: with `-device qemu-xhci -device usb-kbd` on a build
with no USB support, a typed `touch /marker` left no file; without the
USB devices, the identical sequence left one.

So every keystroke assertion here is already controlled. There is no
scaffolding to add and no `i8042=off` needed: if the driver is dead,
nothing types. That is the same argument the Makefile makes for
`DISK=virtio` removing the IDE controller entirely.

THE LOAD-BEARING CHECK IS THE RING WRAP, not the first keystroke. A
driver that ignores the event ring's cycle bit, or never re-posts a
TRB, works perfectly for exactly one lap of the ring -- 256 TRBs, so
about 128 keystrokes -- and then goes permanently deaf. A test that
types a short marker cannot tell that from a working driver. Phase 2
types well past a full ring and asserts the LAST file as well as the
first.

The oracle throughout is the FILESYSTEM, read back over the serial
console, not the screen: a keystroke that worked leaves bytes on disk,
and the serial console does not care who owns the keyboard.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard
import vm as vm_mod
from gui_debug import DebugConsole
from qmp_test import QMPSession

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Enough files that the endpoint's 256-TRB ring wraps more than twice.
# Each `touch /rNN.txt` is 14 keystrokes, so 25 of them is 350 presses
# and 350 releases -- 700 reports against a 256-TRB ring.
WRAP_FILES = 25

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail else ""))
    return ok


class Guest:
    """One booted guest with USB attached, on a copy of disk.img."""

    def __init__(self, instance, usb, kvm=False):
        self.instance = instance
        self.usb = usb
        self.kvm = kvm
        t = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        t.close()
        self.disk = t.name
        subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                        os.path.join(REPO, "disk.img"), self.disk], check=True)

    def _run(self, *args):
        cmd = [sys.executable, os.path.join(REPO, "tools", "vm.py"),
               "--disk", self.disk, "--instance", str(self.instance),
               "--usb", self.usb] + (["--kvm"] if self.kvm else []) + list(args)
        return subprocess.run(cmd, capture_output=True, text=True,
                              cwd=REPO).stdout

    def start(self):
        return vm_mod.started_ok(self._run("start"))

    def stop(self):
        self._run("stop")

    def cleanup(self):
        self.stop()
        try:
            os.unlink(self.disk)
        except OSError:
            pass

    def sock(self):
        return ".vm.serial" if not self.instance else f".vm.{self.instance}.serial"

    def console(self):
        return DebugConsole(os.path.join(REPO, self.sock()))

    def qmp(self):
        return QMPSession(port=4445 + self.instance)


# send_key takes a QEMU qcode, and a character that has no qcode is
# SILENTLY DROPPED -- which is how `touch /usb_one.txt` created
# `usbone.txt` and read as a dead driver. Anything not a bare lowercase
# letter or digit is spelled out here, and a shifted character goes
# through combo() rather than pretending a qcode exists for it.
QCODE = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
SHIFTED = {"_": "minus", ":": "semicolon", "?": "slash", "~": "grave_accent"}


def type_line(qmp, text, delay=0.045):
    for ch in text:
        if ch in SHIFTED:
            qmp.combo(["shift", SHIFTED[ch]])
        elif ch in QCODE:
            qmp.send_key(QCODE[ch])
        elif ch.isalnum() and ch.islower() or ch.isdigit():
            qmp.send_key(ch)
        else:
            raise ValueError(f"type_line: no qcode for {ch!r} -- add one rather "
                             f"than letting it be dropped silently")
        time.sleep(delay)
    qmp.send_key("ret")
    time.sleep(0.9)


def phase_keyboard(instance, kvm=False):
    """The controller, the device, and a keystroke that reaches the shell."""
    print("\nphase 1: keyboard")
    g = Guest(instance, "xhci", kvm)
    try:
        # Boot 1 sets the text target, so boot 2 has a shell prompt to
        # type at rather than the desktop.
        if not g.start():
            return check("guest boots with an xHCI controller", False)
        d = g.console()
        d.send("sh config set system.default_target text")
        d.send("sh rm /usb_one.txt")
        time.sleep(0.4)
        g.stop()

        if not g.start():
            return check("guest reboots into the text target", False)
        d = g.console()
        qmp = g.qmp()
        time.sleep(2.0)

        lsdev = d.send("lsdev") or ""
        check("lsdev lists a usb-keyboard input source",
              "usb-keyboard" in lsdev)
        # EITHER WAY OF BEING INTERRUPT-DRIVEN COUNTS, and the point of
        # the check is the third state: `[polled]`, which is what a
        # controller with no usable interrupt falls back to. It reads
        # `[msi N]` on a machine whose LAPIC came up (the default now)
        # and `[irq N]` on one booted with `nomsi` or without an APIC --
        # asserting the line form alone failed the day the xHCI moved to
        # a vector, on a controller that had become MORE
        # interrupt-driven, not less.
        flat = lsdev.replace("\t", " ")
        check("the controller is interrupt-driven, not polled",
              "usb-xhci  [irq" in flat or "usb-xhci  [msi" in flat,
              next((ln.strip() for ln in flat.splitlines()
                    if "usb-xhci" in ln), "no usb-xhci line"))
        check("PS/2 is still registered beside it",
              "ps2-keyboard" in lsdev and "ps2-mouse" in lsdev)

        dump = d.send("usb") or ""
        # The VID:PID and the product string come off the wire, so they
        # are proof a control transfer round-tripped rather than proof
        # the driver can print a constant.
        check("a control transfer round-tripped (real VID:PID)",
              "0627:0001" in dump, dump.count("0627:0001") and "found" or "missing")
        check("the device's own product string was read",
              "QEMU USB Keyboard" in dump)
        # QEMU reports boot format regardless, so this asserts the
        # REQUEST was issued, which is the only observable difference.
        check("SET_PROTOCOL(boot) was actually issued",
              "1 set-protocol(boot) accepted" in dump or
              "set-protocol(boot) accepted" in dump and
              " 0 set-protocol(boot)" not in dump)

        # The keystroke itself. Nothing but the USB path can deliver it.
        type_line(qmp, "touch /usb_one.txt")
        names = d.send("sh ls /") or ""
        check("a keystroke on the USB keyboard reaches the shell",
              "usb_one.txt" in names,
              "" if "usb_one.txt" in names else f"saw {sorted(n for n in names.split() if n.endswith('.txt'))}")

        dump2 = d.send("usb") or ""
        check("reports were decoded, not merely received",
              " 0 report(s) decoded" not in dump2, dump2.split("report(s)")[0][-12:].strip())
        return True
    finally:
        g.cleanup()


def phase_ring_wrap(instance, kvm=False):
    """Type past a full ring, and assert the LAST keystroke still lands."""
    print(f"\nphase 2: ring wrap ({WRAP_FILES} files, "
          f"~{WRAP_FILES * 14 * 2} reports against a 256-TRB ring)")
    g = Guest(instance, "xhci", kvm)
    try:
        if not g.start():
            return check("guest boots for the wrap test", False)
        d = g.console()
        d.send("sh config set system.default_target text")
        for i in range(WRAP_FILES):
            d.send(f"sh rm /r{i:02d}.txt")
        time.sleep(0.4)
        g.stop()

        if not g.start():
            return check("guest reboots for the wrap test", False)
        d = g.console()
        qmp = g.qmp()
        time.sleep(2.0)

        # DRAIN THE SERIAL SOCKET AS WE GO, or the guest stops.
        #
        # With the text target every `touch` spawns a process, and the
        # kernel logs the spawn and the exit to COM1. Nothing else here
        # reads that socket, so its buffer fills, the guest's serial
        # write blocks, and the whole kernel stalls mid-run -- USB
        # polling included. It presents EXACTLY like a driver that dies
        # after N keystrokes: the guest recovers the instant anything
        # reads the socket, so a dump taken afterwards looks healthy and
        # reports "0 pending".
        #
        # Measured: without this, 5 of 25 files; with it, all 25. The
        # driver was never the variable.
        for i in range(WRAP_FILES):
            type_line(qmp, f"touch /r{i:02d}.txt", delay=0.035)
            if i % 3 == 2:
                d.send("")      # cheap: a bare newline, read to the prompt

        names = d.send("sh ls /") or ""
        missing = [i for i in range(WRAP_FILES) if f"r{i:02d}.txt" not in names]
        if missing:
            print("    saw:", sorted(n for n in names.split() if n.startswith("r") and n.endswith(".txt")))
            print("    listing bytes:", len(names))

        # Asserting the FIRST file only would pass with a dead cycle
        # bit, because the ring does not wrap until well into the run.
        check("the first file was created", 0 not in missing)
        check("THE LAST file was created (the ring wrapped and kept working)",
              (WRAP_FILES - 1) not in missing,
              f"r{WRAP_FILES - 1:02d}.txt")
        check("every file in between was created too",
              not missing, f"missing {missing}" if missing else "all present")

        dump = d.send("usb") or ""
        if missing:
            print("    driver state after the stall:")
            for line in dump.splitlines():
                print("     ", line)
        check("the guest is still answering after the wrap", bool(dump.strip()))
        return True
    finally:
        g.cleanup()


def phase_mouse(instance, kvm=False):
    """The pointer moves, and it moves the right way."""
    print("\nphase 3: mouse")
    g = Guest(instance, "xhci+mouse", kvm)
    try:
        if not g.start():
            return check("guest boots with a usb-mouse", False)
        d = g.console()
        qmp = g.qmp()
        time.sleep(2.5)

        lsdev = d.send("lsdev") or ""
        check("lsdev lists a usb-mouse input source", "usb-mouse" in lsdev)
        dump = d.send("usb") or ""
        check("the mouse enumerated as HID boot mouse (class 3/1/2)",
              "class 3/1/2" in dump)

        def cursor():
            st = d.send("gui state") or ""
            for line in st.splitlines():
                if "cursor" in line:
                    nums = [int(t) for t in line.replace("(", " ").replace(")", " ")
                            .replace(",", " ").split() if t.lstrip("-").isdigit()]
                    if len(nums) >= 2:
                        return nums[0], nums[1]
            return None

        start = cursor()
        if start is None:
            return check("the desktop reports a cursor position", False)

        # QEMU hands pointer events to the USB mouse once our driver has
        # polled that endpoint, so these rel events travel the USB path.
        for _ in range(12):
            qmp.move_rel(10, 8)
            time.sleep(0.05)
        time.sleep(0.6)
        moved = cursor()
        check("the pointer moved", moved is not None and moved != start,
              f"{start} -> {moved}")
        if moved and start:
            # Down in the report must be down on screen. The opposite
            # sign shipped once and a KTEST caught it; this is the same
            # property end to end, through the real device.
            check("a downward report moved the pointer DOWN the screen",
                  moved[1] > start[1], f"y {start[1]} -> {moved[1]}")
            check("a rightward report moved the pointer RIGHT",
                  moved[0] > start[0], f"x {start[0]} -> {moved[0]}")
        return True
    finally:
        g.cleanup()


def input_sources(lsdev_text):
    """Just the `Input sources` section of an lsdev capture.

    The capture carries kernel log lines too, and after an unplug one of
    them IS `input: usb-mouse unregistered` -- so a whole-capture
    substring check reports the source still present at the exact moment
    it was removed. The forbidden-substring-must-be-scoped rule, again.
    """
    lines, taking = [], False
    for ln in (lsdev_text or "").splitlines():
        if ln.startswith("Input sources"):
            taking = True
            continue
        if taking:
            if not ln.startswith(" "):
                break
            lines.append(ln)
    return "\n".join(lines)


def phase_hotplug(instance, kvm=False):
    """A mouse plugged in AFTER boot works, and unplugging it cleans up.

    The port scan used to run exactly once, so this phase is the whole
    hot-plug feature's test: device_add on a RUNNING guest is the only
    headless stand-in for a human plugging a mouse into a laptop.
    """
    print("\nphase 4: hot-plug")
    g = Guest(instance, "xhci", kvm)
    try:
        if not g.start():
            return check("guest boots for the hot-plug test", False)
        d = g.console()
        qmp = g.qmp()
        time.sleep(2.0)

        srcs = input_sources(d.send("lsdev"))
        check("no usb-mouse before the plug (the control)",
              "usb-mouse" not in srcs)

        qmp.device_add("usb-mouse", "hotmouse", bus="xhci.0")
        time.sleep(2.5)          # deferred work runs at idle; generous

        srcs = input_sources(d.send("lsdev"))
        check("a mouse plugged in AFTER boot registers", "usb-mouse" in srcs)
        dump = d.send("usb") or ""
        check("the hot-plugged mouse enumerated (class 3/1/2)",
              "class 3/1/2" in dump)

        # It must WORK, not just enumerate: move the pointer through it.
        d.send("gui")
        time.sleep(2.5)
        def cursor():
            st = d.send("gui state") or ""
            for line in st.splitlines():
                if "cursor" in line:
                    nums = [int(t) for t in line.replace("(", " ").replace(")", " ")
                            .replace(",", " ").split() if t.lstrip("-").isdigit()]
                    if len(nums) >= 2:
                        return nums[0], nums[1]
            return None
        start = cursor()
        for _ in range(10):
            qmp.move_rel(9, 7)
            time.sleep(0.05)
        time.sleep(0.6)
        moved = cursor()
        check("the hot-plugged mouse moves the pointer",
              start is not None and moved is not None and moved != start,
              f"{start} -> {moved}")

        qmp.device_del("hotmouse")
        time.sleep(2.5)
        srcs = input_sources(d.send("lsdev"))
        check("unplugging unregisters the input source",
              "usb-mouse" not in srcs)
        dump = d.send("usb") or ""
        check("the guest is still answering after the unplug", bool(dump.strip()))
        check("the detached device left the table",
              "class 3/1/2" not in dump)
        return True
    finally:
        g.cleanup()


def phase_hub(instance, kvm=False):
    """A keyboard and mouse BEHIND A HUB both work -- the route-string path.

    QEMU's usb-hub is a USB 1.1 full-speed hub, so this exercises hub
    enumeration, per-port power/reset and route strings; what it cannot
    reach is the TT path (low/full behind a HIGH-speed hub), which only
    real hardware presents.
    """
    print("\nphase 5: hub")
    g = Guest(instance, "xhci+hub", kvm)
    try:
        if not g.start():
            return check("guest boots with a hub topology", False)
        d = g.console()
        d.send("sh config set system.default_target text")
        d.send("sh rm /hub_one.txt")
        time.sleep(0.4)
        g.stop()

        if not g.start():
            return check("guest reboots into the text target", False)
        d = g.console()
        qmp = g.qmp()
        time.sleep(2.5)

        dump = d.send("usb") or ""
        check("the hub enumerated (class 9)", "class 9/" in dump)
        check("the keyboard behind the hub enumerated (class 3/1/1)",
              "class 3/1/1" in dump)
        check("the mouse behind the hub enumerated (class 3/1/2)",
              "class 3/1/2" in dump)
        lsdev = d.send("lsdev") or ""
        check("both hub children registered as input sources",
              "usb-keyboard" in lsdev and "usb-mouse" in lsdev)

        # A keystroke THROUGH the hub: only the routed path can deliver
        # it, since QEMU gives the usb-kbd the keyboard the moment it is
        # attached (the tool's self-controlling property, unchanged).
        type_line(qmp, "touch /hub_one.txt")
        names = d.send("sh ls /") or ""
        check("a keystroke through the hub reaches the shell",
              "hub_one.txt" in names)
        return True
    finally:
        g.cleanup()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--phase", choices=("all", "keyboard", "wrap", "mouse",
                                        "hotplug", "hub"),
                    default="all")
    # The INTx storm class this driver's acknowledge path guards against
    # is INVISIBLE under TCG -- virtio-input's version of it hung 3 boots
    # in 3 under KVM and 0 in 3 without. A green TCG run says nothing
    # about it, so this is not optional before believing the driver.
    ap.add_argument("--kvm", action="store_true",
                    help="run with KVM; required to exercise the interrupt "
                         "timing this driver's ack path guards against")
    args = ap.parse_args()

    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("usb_test: no free VM slot")
            return 2
        args.instance = n
        print(f"usb_test: slot {args.instance} (QMP {4445 + args.instance})")
    inst = int(args.instance)

    if not os.path.exists(os.path.join(REPO, "disk.img")):
        print("usb_test: no disk.img -- run `make iso` first")
        return 2

    if args.phase in ("all", "keyboard"):
        phase_keyboard(inst, args.kvm)
    if args.phase in ("all", "wrap"):
        phase_ring_wrap(inst, args.kvm)
    if args.phase in ("all", "mouse"):
        phase_mouse(inst, args.kvm)
    if args.phase in ("all", "hotplug"):
        phase_hotplug(inst, args.kvm)
    if args.phase in ("all", "hub"):
        phase_hub(inst, args.kvm)

    failed = [n for n, ok, _ in results if not ok]
    print(f"\nusb_test: {len(results) - len(failed)}/{len(results)} checks passed")
    if failed:
        print("usb_test: FAILED")
        for n in failed:
            print(f"  - {n}")
        return 1
    print("usb_test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
