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

GUI/rendering work still needs tools/qmp_test.py: a text transcript says
nothing about whether a button is drawn in the right place.
"""

import argparse
import os
import socket
import subprocess
import sys
import time

PIDFILE = ".vm.pid"
SERIAL_SOCK = ".vm.serial"
QMP_PORT = 4445
PROMPT = "dbg> "


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


def cmd_start(args):
    if _read_pid():
        print("vm: already running (vm.py stop first, or vm.py exec ...)")
        return 0
    for stale in (SERIAL_SOCK, PIDFILE):
        if os.path.exists(stale):
            os.unlink(stale)

    cmd = [
        "qemu-system-x86_64",
        "-cdrom", args.iso,
        "-drive", f"file={args.disk},format=raw,if=ide",
        "-m", "256",
    ]
    if args.kvm:
        # Matches `make run-kvm`'s flags, so what this measures is what
        # that target actually does. Off by default because /dev/kvm
        # isn't guaranteed to be readable (CI runners typically have no
        # nested virt at all) and every existing caller expects TCG.
        #
        # Not interchangeable with the default for timing work: KVM runs
        # guest instructions natively but turns every port-I/O access
        # into a hardware VM exit, so `inb`/`outb`-heavy paths can come
        # out SLOWER here. Say which mode a number came from.
        cmd += ["-enable-kvm", "-cpu", "host"]
    elif args.cpu:
        # An explicit QEMU CPU model, for testing code whose behaviour
        # DEPENDS on which CPU is presented. The default (`qemu64`)
        # reports as AuthenticAMD and populates neither CPUID leaf 4 nor
        # 8000001DH, so anything reading cache topology takes the AMD
        # 80000005H/6H fallback there and the leaf-4 path never runs.
        # `--cpu max` or `--cpu Skylake-Client` exercises the other side.
        # Ignored under --kvm, which pins -cpu host by definition.
        cmd += ["-cpu", args.cpu]
    cmd += [
        # A VNC head rather than -display none: input routing needs a
        # display head to exist even when nothing connects to it (see
        # qmp_test.py's docstring), and `vm.py shot` uses QMP.
        "-vga", "std", "-vnc", f":{args.vnc}",
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
    ap.add_argument("--qmp-port", type=int, default=QMP_PORT)
    ap.add_argument("--vnc", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--cpu", default=None,
                     help="QEMU -cpu model (e.g. max, Skylake-Client). The default "
                          "qemu64 reports as AMD and has no CPUID leaf 4, so this is "
                          "how you exercise CPU-model-dependent paths. Ignored with --kvm.")
    ap.add_argument("--kvm", action="store_true",
                    help="use KVM acceleration (like `make run-kvm`) instead of TCG "
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

    p_shot = sub.add_parser("shot", help="screendump to a .png")
    p_shot.add_argument("path")
    p_shot.set_defaults(func=cmd_shot)

    p_run = sub.add_parser("run", help="start, run command(s), stop")
    p_run.add_argument("commands", nargs="+")
    p_run.add_argument("--raw", action="store_true")
    p_run.add_argument("--label", action="store_true")
    p_run.set_defaults(func=cmd_run)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
