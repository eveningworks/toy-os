#!/usr/bin/env python3
"""tools/live_boot_test.py -- the ISO boots and is USABLE with no disk.

WHAT THIS COVERS
----------------
A Live CD boot: GRUB hands the kernel a TFS3 image as a module, the
kernel mounts it through a RAM block device, and the system comes up
with a real /bin and a real /usr (docs/live-cd-design.md). QEMU is
launched with NO -drive at all, which is the point -- an empty disk
image would let the ordinary disk path run and prove nothing.

**Ask what a broken version would still pass.** A live boot that mounted
NOTHING still reaches a shell and still shows a desktop, because the
kernel degrades to an empty RAM filesystem and the WM is compiled in. So
"it booted" and "there is a prompt" are worthless as evidence here. The
assertion has to be that a file the ISO shipped can be read, and that
the volume reports itself honestly.

The four checks:

  1. The RAM block device registered and the filesystem mounted on it.
  2. A file the image SHIPPED is listable -- /bin/wm/apps exists with
     the apps in it. This is the check that separates "mounted the live
     image" from "came up with an empty RAM filesystem".
  3. A ring-3 binary from that image RUNS. Reading a directory only
     proves metadata; executing proves the data blocks are really there
     and really reachable through the RAM device.
  4. It reports itself NOT PERSISTENT. A live volume that claimed
     otherwise would be the most misleading thing this feature could
     do -- a user would expect their files to survive the power going
     off.

    python3 tools/live_boot_test.py
    echo $?
"""

import argparse
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_test import launch_qemu_cmd          # noqa: E402

BOOT_TIMEOUT_S = 40.0
PROMPT = "dbg> "


class Shell:
    """One command at a time over the serial debug console.

    Deliberately its own few lines rather than importing vm.py's: that
    module drives a MANAGED vm (its own pidfile, its own socket path,
    its own disk), and this test's entire point is a QEMU launched
    differently -- no disk at all. Sharing the driver would have meant
    teaching vm.py about a VM it does not own.
    """

    def __init__(self, sock_path, timeout=15.0):
        self.path = sock_path
        self.timeout = timeout

    def run(self, command):
        """`sh <cmd>` is how the debug console reaches the shell -- see
        tools/vm.py, which drives the same wire."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(self.timeout)
            s.connect(self.path)
            # A bare newline first: synchronise on a fresh prompt rather
            # than trying to match whatever the boot printed.
            s.sendall(b"\n")
            buf = ""
            deadline = time.time() + self.timeout
            while time.time() < deadline and not buf.endswith(PROMPT):
                buf += s.recv(65536).decode("utf-8", errors="replace")
            s.sendall((command + "\n").encode())
            out = ""
            deadline = time.time() + self.timeout
            while time.time() < deadline and not out.endswith(PROMPT):
                out += s.recv(65536).decode("utf-8", errors="replace")
            if out.startswith(command):
                out = out[len(command):]
            return out[:-len(PROMPT)] if out.endswith(PROMPT) else out

    def close(self):
        pass


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # The LIVE iso, which is its own artifact (`make live-iso`) -- the
    # ordinary one carries no module, deliberately, because GRUB reads
    # the whole thing off the CD before the kernel starts and that cost
    # belongs to the image that needs it. See the Makefile.
    ap.add_argument("--iso", default="toy-os-live.iso")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"live_boot_test: {args.iso} not found -- run `make live-iso` first")
        return 1

    serial = os.path.abspath(os.path.join(args.tmp, "live_serial.log"))
    sock = os.path.abspath(os.path.join(args.tmp, "live.serial"))
    pidfile = os.path.abspath(os.path.join(args.tmp, "live_qemu.pid"))
    for f in (serial, pidfile):
        if os.path.exists(f):
            os.remove(f)

    # NO DISK. launch_qemu_cmd(disk=None) omits -drive entirely.
    cmd = launch_qemu_cmd(iso=args.iso, disk=None, serial_log=serial,
                          qmp_port=4499, vnc_display=9, pidfile=pidfile)
    # A unix SOCKET rather than the helper's log file: this test drives
    # the console interactively, and `-serial file:` is write-only.
    cmd = cmd.replace(f"-serial file:{serial}", f"-serial unix:{sock},server,nowait")
    subprocess.run(cmd, shell=True, check=True)

    res = Result()
    sh = None
    try:
        deadline = time.time() + BOOT_TIMEOUT_S
        while time.time() < deadline:
            try:
                # Short timeout for the PROBE only -- it is asked
                # repeatedly while the guest boots. The real shell gets
                # the full one, or a slow command comes back truncated
                # and reads as a wrong answer rather than a slow one.
                Shell(sock, timeout=3.0).run("")
                sh = Shell(sock)
                break
            except Exception:
                time.sleep(0.5)
        if sh is None:
            res.check("the ISO booted with no disk", False,
                      f"no serial console within {BOOT_TIMEOUT_S}s")
            raise SystemExit(1)

        # 1. mounted from RAM
        df = sh.run("sh df")
        # Mounted AND with content: a 130 MB volume with 4 MB used is
        # the live image. An empty RAM filesystem reports the same
        # backend name, so the name alone proves nothing.
        used_kb = 0
        for line in df.splitlines():
            if "used:" in line:
                used_kb = int("".join(c for c in line if c.isdigit()) or 0)
        res.check("the filesystem mounted from the live image",
                  "tfs3" in df.lower() and used_kb > 1000,
                  f"{used_kb} KB used -- an empty RAM filesystem, not the image")

        # 2. shipped content is there -- the check that separates a live
        #    mount from an empty RAM filesystem
        ls = sh.run("sh ls /bin/wm/apps")
        res.check("a directory the image shipped is readable",
                  "calculator" in ls and "notepad" in ls, ls.strip()[:200])

        # 3. and its data blocks really are reachable
        out = sh.run("sh run libc_test")
        res.check("a binary from the live image runs",
                  "PASS" in out or "pass" in out, out.strip()[:200])

        # 4. and it says so honestly
        # In df's own words: "RAM-only -- won't survive reboot". A live
        # volume claiming persistence would tell a user their files are
        # safe when the next power cycle erases them.
        res.check("the volume reports itself NOT persistent",
                  "ram-only" in df.lower(), df.strip()[:160])
    finally:
        if sh:
            sh.close()
        if os.path.exists(pidfile):
            with open(pidfile) as f:
                pid = f.read().strip()
            if pid:
                subprocess.run(f"kill {pid}", shell=True)

    print(f"\nlive_boot_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
