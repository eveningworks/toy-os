#!/usr/bin/env python3
"""Manage a headless toy-os VM and talk to it in TEXT.

This exists because driving toy-os for verification used to mean: write
a `qemu-system-x86_64 ... -daemonize -pidfile` line by hand, sleep, open
a QMP session, emulate keystrokes one qcode at a time, screenshot, and
read the PNG with your eyes. That works, but it depends on the guest's
keyboard layout (a Swedish layout turns `write_test` into `write?test`),
drops keys under load, and produces a picture rather than an assertion.

With `sh` on the serial debug console (kernel/core/debug_console.c),
shell output comes back as text on a socket, so a check is
`assert "PASSED" in out` instead of a screenshot to squint at.

    python3 tools/vm.py start            # boot and wait until ready
    python3 tools/vm.py exec "ls /bin" "df"
    python3 tools/vm.py shot out.png     # still there when you want pixels
    python3 tools/vm.py status
    python3 tools/vm.py stop

    python3 tools/vm.py run "fsck"       # start, exec, stop -- one shot

Safety: this only ever kills a QEMU whose PID it wrote to its own
pidfile (.vm.pid), so an interactive `make run` window is never at risk
-- the mistake CLAUDE.md warns about with `pkill -f qemu-system-x86_64`.

Several VMs can run at once, each in its own slot:

    python3 tools/vm.py --instance 2 --disk /tmp/b.img start
    python3 tools/vm.py --instance 2 exec "df"

`--instance N` derives the pidfile, serial socket, QMP port and VNC
display from N (see _apply_instance), so slot 2 can never stop slot 0's
VM or connect to its console. Slot 0 is the default and is unchanged.

GUI/rendering work still needs tools/qmp_test.py: a text transcript says
nothing about whether a button is drawn in the right place.
"""

import argparse
import os
import socket
import subprocess
import sys
import time
import install_grub
import iso_guard
import port_guard

PIDFILE = ".vm.pid"
SERIAL_SOCK = ".vm.serial"
QMP_PORT = 4445
VNC_DISPLAY = 5
PROMPT = "dbg> "


def _instance_arg(v):
    """`--instance N`, or `auto` to take the lowest free slot."""
    if v == "auto":
        return "auto"
    return int(v)


def _apply_instance(args):
    """Derive every per-VM resource from one slot number.

    Several VMs can run side by side (tools/gui_regress.py runs the GUI
    test tools in parallel, one slot each), and each needs its own
    pidfile, serial socket, QMP port and VNC display. Deriving all four
    from a single `--instance N` keeps them from being mixed up: slot 3
    is always `.vm.3.pid`/`.vm.3.serial`/port 4448/display :8, so a
    failing parallel run can be re-driven by hand with the same numbers.

    Derived rather than allocated, by default, for two reasons: a probe
    has a bind/close race, and a port that differs on every run makes a
    failure harder to reproduce than it needs to be.

    `--instance auto` opts into picking the lowest FREE slot, and
    answers the second objection by PRINTING the number it chose, so a
    failure is replayed with `--instance <that number>`. It does not
    answer the first -- see port_guard.find_free_instance(), which is
    explicitly not a lock -- which is why assert_ports_free() still runs
    at the launch itself. Reach for it when something else may already
    be running (a gui_regress.py in another terminal); leave it alone
    for a plain interactive session.

    Slot 0 keeps the original, unsuffixed names and the original port,
    so every existing caller and every test tool's default still works
    untouched.
    """
    global PIDFILE, SERIAL_SOCK
    n = getattr(args, "instance", 0) or 0
    if n == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("vm: no free slot -- 16 guests are already running?", file=sys.stderr)
            raise SystemExit(2)
        print(f"vm: instance {n} (QMP {QMP_PORT + n}) -- re-run with "
              f"--instance {n} to reach this guest")
    n = int(n)
    args.instance = n
    if n:
        PIDFILE = f".vm.{n}.pid"
        SERIAL_SOCK = f".vm.{n}.serial"
    if getattr(args, "qmp_port", None) is None:
        args.qmp_port = QMP_PORT + n
    if getattr(args, "vnc", None) is None:
        args.vnc = VNC_DISPLAY + n


def _running(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _read_pid():
    if not os.path.exists(PIDFILE):
        return None
    try:
        pid = int(open(PIDFILE).read().strip())
    except (ValueError, OSError):
        return None
    if not _running(pid):
        # Stale pidfile from a VM that died or was killed elsewhere.
        # Clearing it here is what stops the next `start` failing with
        # QEMU's "cannot create PID file: Cannot lock pid file".
        os.unlink(PIDFILE)
        return None
    return pid


# The one line every caller uses to decide whether `start` worked.
#
# **`"ready" in output` IS WRONG, AND IT LOOKS RIGHT.** The failure
# message for a slot that is already taken is "vm: already running", and
# "al-ready" CONTAINS "ready" -- so the obvious test passes on the one
# output that means the opposite, and a tool then talks to somebody
# else's guest (or to a serial socket that is not there) for reasons that
# surface much later and point somewhere else. Eight tools here carried
# that test; this exists so there is one place to be right.
#
# Takes the combined stdout+stderr of a `vm.py ... start`, since that is
# what a subprocess caller has.
def started_ok(output):
    return "vm: ready" in (output or "")


def cmd_start(args):
    if _read_pid():
        print("vm: already running (vm.py stop first, or vm.py exec ...)")
        return 0
    for stale in (SERIAL_SOCK, PIDFILE):
        if os.path.exists(stale):
            os.unlink(stale)

    # WHICH MEDIUM. The kernel is on disk.img now (tools/install_grub.py),
    # so the disk boots itself and the ISO is for the images that have no
    # disk -- live, demo, a release download. Derived rather than fixed
    # because an image built before the boot partition existed has no
    # GRUB on it and must keep working; `--boot` overrides.
    medium = install_grub.boot_medium(args.disk, args.boot)

    # A stale image boots and PASSES, silently testing the previous
    # build -- see tools/iso_guard.py. Checked here because vm.py and
    # qmp_test.py's launch_qemu_cmd() are the two places anything in
    # this repo starts a guest.
    if args.iso == "toy-os.iso":
        iso_guard.assert_iso_fresh(medium=medium)

    # And a COPY of disk.img taken before the last seed, which runs the
    # new kernel against the OLD /bin binaries. A warning, not a refusal:
    # a copy is often deliberately old (a staged fixture, a kept
    # reproduction). See iso_guard.check_disk_fresh().
    iso_guard.warn_if_disk_stale(args.disk)

    # Likewise a guest already holding this slot's ports -- see
    # tools/port_guard.py for why that has to be refused HERE rather
    # than surfacing later as somebody else's broken socket.
    port_guard.assert_ports_free(args.qmp_port, args.vnc)

    cmd = [
        "qemu-system-x86_64",
        # `order=c` boots the disk's own GRUB; `order=d` boots the ISO
        # and leaves the disk as data. Never NO order: SeaBIOS tries the
        # hard disk first whenever it looks bootable, and a partition
        # table is all it looks for -- it then jumps into 446 bytes of
        # table as if they were boot code and hangs with no serial output
        # at all, which reads as a kernel that died before its first
        # print rather than as one that never ran.
        *install_grub.qemu_boot_args(medium, args.iso),
        "-m", str(getattr(args, "mem", 0) or 2048),
    ]
    # THE CHIPSET. Default (i440fx) unless asked, because that is what
    # every existing test was written against. `--machine q35` is the
    # only way to reach an ACPI 2.0 machine here: i440fx presents a
    # revision-0 RSDP with an RSDT and no reset register, q35 presents a
    # revision-2 RSDP with an XSDT and a real one, so the two exercise
    # different halves of kernel/acpi/. q35 has NO legacy IDE, so it
    # implies --disk-kind ahci rather than silently booting no disk.
    if getattr(args, "machine", None):
        cmd += ["-machine", args.machine]
        if args.machine.startswith("q35"):
            args.disk_kind = "ahci"
    # THE BOOT DISK, and which controller it hangs off. `--disk-kind
    # ahci` is the only way anything here reaches kernel/drivers/ahci.c,
    # the same reason --vga virtio and --virtio-input exist; the
    # Makefile's DISK= axis is its interactive twin.
    # discard=unmap turns the guest's TRIM into a hole punch in the
    # backing file -- see kernel/drivers/ata.c's ata_trim().
    if getattr(args, "disk_kind", "ide") == "ahci":
        cmd += ["-device", "ich9-ahci,id=ahci",
                "-drive", f"file={args.disk},format=raw,if=none,id=sata0,discard=unmap",
                "-device", "ide-hd,drive=sata0,bus=ahci.0"]
    else:
        cmd += ["-drive", f"file={args.disk},format=raw,if=ide,discard=unmap"]
    if args.kvm:
        # Matches `make run KVM=1`'s flags, so what this measures is what
        # that target actually does. Off by default because /dev/kvm
        # isn't guaranteed to be readable (CI runners typically have no
        # nested virt at all) and every existing caller expects TCG.
        #
        # Not interchangeable with the default for timing work: KVM runs
        # guest instructions natively but turns every port-I/O access
        # into a hardware VM exit, so `inb`/`outb`-heavy paths can come
        # out SLOWER here. Say which mode a number came from.
        #
        # `--cpu` still applies here, and there is one case that NEEDS
        # it: QEMU withholds `invtsc` (CPUID 8000_0007H EDX bit 8, the
        # invariant-TSC bit) even under `-cpu host`, because a guest
        # that has seen it cannot be live-migrated. So the kernel's TSC
        # clocksource is unreachable until you ask:
        #
        #     python3 tools/vm.py --kvm --cpu host,+invtsc start
        #
        # Plain TCG cannot reach it at all -- `-cpu max,+invtsc` warns
        # "TCG doesn't support requested feature" and clears the bit --
        # so that command line is the ONLY way to exercise TSC-backed
        # timekeeping in this environment. See docs/boot-flags.md's
        # `notsc`, which reaches the other path from the other side.
        cmd += ["-enable-kvm", "-cpu", args.cpu or "host"]
    elif args.cpu:
        # An explicit QEMU CPU model, for testing code whose behaviour
        # DEPENDS on which CPU is presented. The default (`qemu64`)
        # reports as AuthenticAMD and populates neither CPUID leaf 4 nor
        # 8000001DH, so anything reading cache topology takes the AMD
        # 80000005H/6H fallback there and the leaf-4 path never runs.
        # `--cpu max` or `--cpu Skylake-Client` exercises the other side.
        cmd += ["-cpu", args.cpu]
    # An optional SECOND disk on virtio, off by default so every existing
    # invocation is byte-for-byte unchanged. `if=none` plus an explicit
    # -device rather than `if=virtio`, so disable-legacy can be chosen:
    # bare `if=virtio` on pc-i440fx yields a TRANSITIONAL device
    # (1af4:1001), which the driver does handle, but the modern form is
    # what the transport is written against.
    if getattr(args, "virtio_disk", None):
        # discard=unmap for parity with the Makefile's DISK=virtio line.
        # Without it QEMU advertises max_discard_sectors as ZERO, the
        # driver correctly reports that it cannot discard, and the whole
        # TRIM path is silently untestable through this tool.
        cmd += ["-drive", f"file={args.virtio_disk},format=raw,if=none,id=vblk,discard=unmap",
                "-device", "virtio-blk-pci,drive=vblk,disable-legacy=on"]
    # virtio input devices, off by default so every existing test keeps
    # the PS/2 pair it was written against. With this the guest gets a
    # keyboard, a relative mouse and an absolute tablet on the virtio
    # transport -- which is the only way to reach
    # kernel/drivers/virtio/virtio_input.c at all, the same reason
    # --vga virtio exists for the GPU.
    if getattr(args, "virtio_input", False):
        cmd += ["-device", "virtio-keyboard-pci",
                "-device", "virtio-mouse-pci",
                "-device", "virtio-tablet-pci"]
    # An AC97 controller whose output QEMU RECORDS to a host wav file --
    # the oracle tools/audio_test.py measures a played tone in. Off by
    # default: attaching audio hardware changes the PCI layout under
    # every existing test for a device none of them drive.
    if getattr(args, "audio_wav", None):
        cmd += ["-audiodev", f"wav,id=snd0,path={args.audio_wav}",
                "-device", "AC97,audiodev=snd0"]
    # An xHCI controller plus USB HID devices, off by default for the
    # same reason --virtio-input is: every existing test was written
    # against the PS/2 pair. `--usb xhci` attaches a keyboard, which
    # does NOT disturb pointer routing; `--usb xhci+mouse` adds a
    # pointer, and QEMU activates a pointer handler lazily on the first
    # poll -- so from the moment kernel/drivers/usb/ polls that
    # endpoint, QMP `rel` events drive the USB mouse instead of the PS/2
    # one. That is exactly what a mouse test wants and exactly why this
    # is not on by default (see docs/testing.md).
    #
    # Devices are given ids so a test can aim `input-send-event` at one
    # by name rather than relying on which handler QEMU picked.
    usb = getattr(args, "usb", "none") or "none"
    if usb != "none":
        cmd += ["-device", "qemu-xhci,id=xhci"]
        if usb == "xhci+hub":
            # The keyboard AND mouse both sit BEHIND a usb-hub (QEMU's
            # is a USB 1.1 full-speed hub), which is the only headless
            # way to exercise route strings and the hub class driver.
            cmd += ["-device", "usb-hub,id=usbhub,port=1,bus=xhci.0",
                    "-device", "usb-kbd,id=usbkbd,bus=xhci.0,port=1.1",
                    "-device", "usb-mouse,id=usbmouse,bus=xhci.0,port=1.2"]
        else:
            cmd += ["-device", "usb-kbd,id=usbkbd,bus=xhci.0"]
            if usb == "xhci+mouse":
                cmd += ["-device", "usb-mouse,id=usbmouse,bus=xhci.0"]
    # WHICH NIC, and why the default is spelled out rather than left
    # implicit. QEMU's default pc machine already attaches an e1000 with
    # user-mode networking when no -net/-netdev option is given -- every
    # guest this harness has ever launched had one -- so `e1000` here is
    # what was already happening, named. `virtio` swaps it for
    # virtio-net-pci, which is the ONLY way to reach
    # kernel/drivers/virtio/virtio_net.c; `both` attaches one of each,
    # the multi-NIC shape nothing else here boots; `none` gives the
    # guest no card at all, which is what makes "no device" a testable
    # state rather than an assumption.
    net = getattr(args, "net", "e1000") or "e1000"
    # A HOST PORT ONTO A GUEST PORT, which is the only way anything can
    # start a conversation WITH the guest: SLIRP is a NAT, so outbound
    # works with no configuration and inbound needs this. Required by
    # anything testing a server in the guest, and by the ICMP
    # port-unreachable check, which needs a datagram to ARRIVE.
    # QEMU spells it `hostfwd=SPEC`; the flag takes the SPEC alone so a
    # caller writes what the QEMU documentation calls it.
    fwd = "".join(",hostfwd=" + f for f in (getattr(args, "hostfwd", None) or []))
    if net == "none":
        cmd += ["-nic", "none"]
    else:
        if net in ("e1000", "both"):
            cmd += ["-netdev", f"user,id=n0{fwd}", "-device", "e1000,netdev=n0"]
        if net in ("virtio", "both"):
            cmd += ["-netdev", "user,id=n1",
                    "-device", "virtio-net-pci,netdev=n1,disable-legacy=on"]
    cmd += [
        # A VNC head rather than -display none: input routing needs a
        # display head to exist even when nothing connects to it (see
        # qmp_test.py's docstring), and `vm.py shot` uses QMP.
        # `--vga vmware` is not cosmetic: vmsvga is the only driver that
        # declares DISPLAY_CAP_CURSOR and the only one that can
        # modeset, so the hardware-cursor and modesetting paths are
        # UNREACHABLE under the default `std` adapter. Same rule as
        # `--cpu` above and `ata nodma` -- a fallback nothing can reach
        # is a guess.
        "-vga", args.vga, "-vnc", f":{args.vnc}",
        # A unix socket for COM1, with nowait so the guest boots
        # immediately rather than waiting for a client. Anything printed
        # before the first connect is lost, which is fine: exec() gets a
        # fresh prompt by sending a newline rather than by matching the
        # boot banner.
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-qmp", f"tcp:127.0.0.1:{args.qmp_port},server,nowait",
        "-daemonize", "-pidfile", PIDFILE,
        "-no-reboot",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("vm: QEMU failed to start:\n" + (r.stderr or r.stdout).strip())
        return 1

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        try:
            out = _exec_one("", timeout=2.0)  # bare newline -> prompt
            if out is not None:
                print(f"vm: ready (pid {_read_pid()})")
                return 0
        except OSError:
            pass
        time.sleep(0.3)
    print(f"vm: started but never reached the debug console within {args.timeout}s")
    return 1


def _exec_one(command, timeout=15.0):
    """Sends one command, returns its output text (without the prompt).

    Returns None if the console didn't answer in time -- the caller
    decides whether that's "still booting" or a real failure.
    """
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(SERIAL_SOCK)
        # A bare newline first: whatever the guest printed before this
        # connection is gone, so synchronise on a fresh prompt instead of
        # trying to match the boot banner.
        s.sendall(b"\n")
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline and not buf.endswith(PROMPT):
            try:
                buf += s.recv(65536).decode("utf-8", errors="replace")
            except socket.timeout:
                return None
        if not buf.endswith(PROMPT):
            return None
        if command == "":
            return ""

        s.sendall((command + "\n").encode())
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline and not buf.endswith(PROMPT):
            try:
                buf += s.recv(65536).decode("utf-8", errors="replace")
            except socket.timeout:
                break
        # Strip the echoed command line and the trailing prompt.
        text = buf.replace("\r", "")
        if text.startswith(command):
            text = text[len(command):]
        if text.endswith(PROMPT):
            text = text[: -len(PROMPT)]
        return text.strip("\n")


def cmd_exec(args):
    if not _read_pid():
        print("vm: not running (vm.py start first)")
        return 1
    failed = 0
    for command in args.commands:
        out = _exec_one(f"sh {command}" if not args.raw else command, timeout=args.timeout)
        if out is None:
            print(f"--- {command} ---\nvm: no response within {args.timeout}s")
            failed = 1
            continue
        if len(args.commands) > 1 or args.label:
            print(f"--- {command} ---")
        print(out)
    return failed


# Lines the KERNEL writes onto the same serial channel a command's
# output comes back on. They appear around every command regardless of
# what it did, so anything deciding "is there output yet" has to drop
# them first -- see cmd_spawn()'s polling loop for what happens
# otherwise.
_NOISE_PREFIXES = (
    "elf_run:", "syscall:", "sh ", "cat:", "vm:",
)


def _strip_kernel_noise(text):
    if not text:
        return ""
    keep = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if any(stripped.startswith(p) for p in _NOISE_PREFIXES):
            continue
        keep.append(line)
    return "\n".join(keep)


def cmd_spawn(args):
    """`spawn` a program and read the file it writes its output to.

    THE DANCE THIS REPLACES, which was hand-rolled four times in one
    session: `exec "spawn /tests/x"`, sleep for a guessed number of
    seconds, then `exec "cat /tmp/x.out"`. Three commands, one of which
    is a magic number.

    Why the file at all: a SPAWNED program's console output arrives
    while this harness is between commands, where it is dropped -- so
    every spawned test here writes its verdict to /tmp and the harness
    reads that instead. Waiting on the ARTIFACT rather than on a sleep
    is this repo's own rule (see CLAUDE.md), and it is what makes this
    reliable rather than merely shorter: it polls until the file
    appears and stops growing.

    Why spawn rather than run: the legacy `run` loader has no scheduler
    slot, so a program that blocks (on a pipe, on a child) or asks for
    its own pid (clock()) cannot work under it.
    """
    if not _read_pid():
        print("vm: not running (vm.py start first)")
        return 1

    out_path = args.out or ("/tmp/" + os.path.basename(args.path) + ".out")
    # Remove any leftover first: a stale file from a previous run is
    # indistinguishable from a fresh one, and would be reported as this
    # run's result.
    _exec_one(f"sh rm {out_path}", timeout=args.timeout)

    cmd = f"spawn {args.path}" + (f" {args.args}" if args.args else "")
    started = _exec_one(f"sh {cmd}", timeout=args.timeout) or ""
    if "started as pid" not in started:
        print(f"vm: spawn failed\n{started}")
        return 1

    # Poll for the artifact, then for it to STOP GROWING -- a report
    # written in pieces would otherwise be read half-finished, which is
    # the same settled-frame problem screendumps have.
    #
    # THE KERNEL TALKS ON THE SAME CHANNEL, and that is not cosmetic:
    # every `cat` prints "elf_run: calling process_run_ring3()" whether
    # or not the file exists, so an unfiltered comparison sees stable
    # output immediately and reports the noise as the result. The first
    # version of this did exactly that and returned three kernel lines
    # for a benchmark that had not finished running.
    deadline = time.time() + args.wait
    last = None
    stable = 0
    while time.time() < deadline:
        time.sleep(0.5)
        raw = _exec_one(f"sh cat {out_path}", timeout=args.timeout)
        body = _strip_kernel_noise(raw)
        if not body:
            continue
        if body == last:
            stable += 1
            if stable >= 2:
                print(body)
                return 0
        else:
            stable = 0
            last = body
    if last is not None:
        print(last)
        print(f"vm: WARNING -- {out_path} was still changing after {args.wait}s")
        return 0
    print(f"vm: {out_path} never appeared within {args.wait}s -- "
          f"does {args.path} write one? (see tools/usertest_run.py's TESTS table)")
    return 1


def cmd_shot(args):
    if not _read_pid():
        print("vm: not running (vm.py start first)")
        return 1
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from qmp_test import QMPSession
    QMPSession(port=args.qmp_port).screenshot(args.path)
    print(args.path)
    return 0


def cmd_status(args):
    pid = _read_pid()
    print(f"vm: running (pid {pid})" if pid else "vm: not running")
    return 0


def cmd_stop(args):
    pid = _read_pid()
    if not pid:
        print("vm: not running")
        return 0
    os.kill(pid, 15)
    for _ in range(50):
        if not _running(pid):
            break
        time.sleep(0.1)
    for f in (PIDFILE, SERIAL_SOCK):
        if os.path.exists(f):
            os.unlink(f)
    print("vm: stopped")
    return 0


def cmd_run(args):
    rc = cmd_start(args)
    if rc:
        return rc
    try:
        return cmd_exec(args)
    finally:
        cmd_stop(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="toy-os.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--boot", choices=("auto", "disk", "cd"), default="auto",
                    help="which medium to boot: the disk's own GRUB, the ISO, "
                         "or (default) whichever the image supports")
    ap.add_argument("--instance", type=_instance_arg, default=0, metavar="N",
                    help="run as VM slot N: pidfile .vm.N.pid, socket .vm.N.serial, "
                         f"QMP port {QMP_PORT}+N, VNC :{VNC_DISPLAY}+N. Slot 0 (the "
                         "default) keeps the original unsuffixed names. Lets several "
                         "VMs run side by side -- see tools/gui_regress.py -j.")
    ap.add_argument("--qmp-port", type=int, default=None,
                    help=f"override the QMP port (default: {QMP_PORT} + --instance)")
    ap.add_argument("--vnc", type=int, default=None,
                    help=f"override the VNC display (default: {VNC_DISPLAY} + --instance)")
    ap.add_argument("--mem", type=int, default=0, metavar="MIB",
                    help="guest RAM in MiB (default 2048). Smaller makes "
                          "memory exhaustion reachable -- see tools/mem_stress.py")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--machine", default=None, metavar="TYPE",
                    help="QEMU machine type (e.g. q35). q35 implies --disk-kind ahci; "
                         "it is the ACPI 2.0 / XSDT / reset-register machine.")
    ap.add_argument("--disk-kind", choices=("ide", "ahci"), default="ide",
                    help="which controller --disk hangs off; ahci is an ICH9 HBA "
                         "and is what exercises kernel/drivers/ahci.c")
    ap.add_argument("--virtio-disk", default=None,
                    help="attach PATH as a virtio-blk disk. Off by default; with it "
                         "the guest gets a second disk on virtio-blk-pci, which is "
                         "what exercises the virtio transport and virtqueue.")
    ap.add_argument("--audio-wav", default=None, metavar="PATH",
                    help="attach an AC97 whose output records to this host wav")
    ap.add_argument("--hostfwd", action="append", metavar="SPEC",
                    help="a QEMU hostfwd rule, e.g. tcp::8080-:80 -- repeatable. "
                         "The only way to reach a server INSIDE the guest, since "
                         "SLIRP is a NAT")
    ap.add_argument("--net", choices=("e1000", "virtio", "both", "none"), default="e1000",
                    help="which NIC to attach. e1000 is what QEMU already did "
                         "implicitly; virtio reaches the virtio-net driver; both "
                         "is the multi-NIC case; none means no card at all")
    ap.add_argument("--virtio-input", action="store_true",
                    help="attach virtio keyboard/mouse/tablet devices. Off by "
                         "default; with it the guest has BOTH these and the PS/2 "
                         "pair, which is what exercises the input core's "
                         "multiple-source path.")
    ap.add_argument("--usb", choices=("none", "xhci", "xhci+mouse", "xhci+hub"),
                    default="none",
                    help="attach an xHCI controller and USB HID devices. Off by "
                         "default; `xhci` adds a usb-kbd, `xhci+mouse` adds a "
                         "usb-mouse too, `xhci+hub` puts a keyboard AND mouse "
                         "behind a usb-hub (the route-string path). A named "
                         "value rather than a boolean because the mouse changes "
                         "QMP pointer routing once the guest driver polls it.")
    ap.add_argument("--vga", default="std",
                    help="QEMU -vga adapter (std, vmware, ...). All three of std, "
                         "vmware and virtio have a modesetting driver now (std "
                         "through kernel/drivers/display/bochs.c), so `video=` in "
                         "the ISO's KCMDLINE works on any of them; `vmware` is the "
                         "only one offering a HARDWARE cursor, so that path is "
                         "unreachable under the default `std`.")
    ap.add_argument("--cpu", default=None,
                     help="QEMU -cpu model (e.g. max, Skylake-Client). The default "
                          "qemu64 reports as AMD and has no CPUID leaf 4, so this is "
                          "how you exercise CPU-model-dependent paths. Ignored with --kvm.")
    ap.add_argument("--kvm", action="store_true",
                    help="use KVM acceleration (like `make run KVM=1`) instead of TCG "
                         "emulation; needs /dev/kvm. Timing numbers from the two modes "
                         "are not comparable -- see cmd_start().")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("start").set_defaults(func=cmd_start)
    sub.add_parser("stop").set_defaults(func=cmd_stop)
    sub.add_parser("status").set_defaults(func=cmd_status)

    p_exec = sub.add_parser("exec", help="run shell command(s) and print their output")
    p_exec.add_argument("commands", nargs="+")
    p_exec.add_argument("--raw", action="store_true",
                        help="send to the debug console directly instead of wrapping in `sh`")
    p_exec.add_argument("--label", action="store_true", help="always print a --- command --- header")
    p_exec.set_defaults(func=cmd_exec)

    p_spawn = sub.add_parser("spawn",
                              help="spawn a program and print the file it writes")
    p_spawn.add_argument("path", help="guest path, e.g. /tests/env_test")
    p_spawn.add_argument("args", nargs="?", default=None, help="arguments, as one string")
    p_spawn.add_argument("--out", default=None,
                          help="the file it writes (default /tmp/<name>.out)")
    p_spawn.add_argument("--wait", type=float, default=30.0,
                          help="seconds to wait for the artifact to settle")
    p_spawn.set_defaults(func=cmd_spawn)

    p_shot = sub.add_parser("shot", help="screendump to a .png")
    p_shot.add_argument("path")
    p_shot.set_defaults(func=cmd_shot)

    p_run = sub.add_parser("run", help="start, run command(s), stop")
    p_run.add_argument("commands", nargs="+")
    p_run.add_argument("--raw", action="store_true")
    p_run.add_argument("--label", action="store_true")
    p_run.set_defaults(func=cmd_run)

    args = ap.parse_args()
    _apply_instance(args)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
