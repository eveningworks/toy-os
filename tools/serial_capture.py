#!/usr/bin/env python3
"""tools/serial_capture.py -- read a running VM's serial console RAW.

WHY THIS EXISTS, AND WHEN gui_debug.py CANNOT DO IT
---------------------------------------------------
`DebugConsole` is a request/response channel: it sends a command and
reads until the prompt comes back. That is the right shape for almost
everything and the wrong shape for exactly one case -- **a kernel that
is dying.** A panicking kernel never returns a prompt, so every read
looks like a timeout and the panic block itself is discarded as noise.
`DebugConsole.capture_panic()` covers the case where the console is
still answering; it cannot cover a command that kills the machine
outright, because the console object is gone with it.

This connects to the same socket, dumps EVERYTHING it receives, and
optionally sends one command first. That is the tool you want for:

  * a command that panics the guest (this was written after a
    `gui spawn /bin/config set ...` took the kernel down with a #GP,
    and the panic -- relocation offset, link-time RIP, stack scan --
    only exists on the serial wire);
  * boot output, including anything printed before the console is up;
  * watching a long-running or intermittent failure with a capture that
    was already running, which `docs/roadmap.md` asks for by name.

**Only one reader.** The serial socket delivers each byte once, so this
and a `DebugConsole` cannot both be attached -- the second gets nothing
and looks hung. Drive input over QMP (`--gui`) rather than opening a
second console, or send it through this tool's own `--send`.

USAGE
-----
    # everything the guest says, until Ctrl-C
    python3 tools/serial_capture.py

    # enter the desktop over QMP, then run a command that may panic
    python3 tools/serial_capture.py --gui --send "gui spawn /bin/config set cursor_size normal"

    # a different VM slot (vm.py --instance N), to a file
    python3 tools/serial_capture.py --sock .vm.2.serial --out /tmp/panic.txt

Pipe the result straight into `tools/panic_resolve.py` to name the
addresses -- it reads the build id first and refuses to be quietly
wrong about a different build.
"""

import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402


def capture(sock_path, seconds, send=None, settle=2.0, enter_gui=False,
            qmp_port=4445, out=None):
    if enter_gui:
        from qmp_test import QMPSession
        from gui_debug import enter_gui as _enter_gui
        qmp = QMPSession(port=qmp_port)
        _enter_gui(qmp, sock=sock_path)   # polls readiness; types nothing if up

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(0.5)

    def drain(secs, sink):
        end = time.time() + secs
        while time.time() < end:
            try:
                data = s.recv(65536)
                if not data:
                    return False
                sink.append(data)
            except socket.timeout:
                pass
        return True

    # Whatever is already queued is not what we asked for -- discard it
    # so the capture starts at the command, not mid-sentence.
    pre = []
    drain(settle, pre)

    got = []
    if send:
        s.sendall((send + "\n").encode())
    drain(seconds, got)

    text = b"".join(got).decode("utf-8", "replace")
    if out:
        with open(out, "w") as f:
            f.write(text)
        print(f"serial_capture: {len(text)} bytes -> {out}", file=sys.stderr)
    return text


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="how long to capture after sending (default 10)")
    ap.add_argument("--send", default=None,
                    help="one console command to send first")
    ap.add_argument("--gui", action="store_true",
                    help="type `gui` over QMP first, to bring the desktop up")
    ap.add_argument("--out", default=None, help="write the capture here too")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "serial_capture")

    text = capture(args.sock, args.seconds, send=args.send,
                   enter_gui=args.gui, qmp_port=args.qmp_port, out=args.out)
    sys.stdout.write(text)
    # A panic is worth an exit code: a capture that caught one is not a
    # successful run of whatever was being tested.
    return 2 if "PANIC:" in text else 0


if __name__ == "__main__":
    sys.exit(main())
