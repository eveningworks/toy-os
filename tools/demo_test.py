#!/usr/bin/env python3
"""tools/demo_test.py -- the scripted demo tour actually performs.

WHAT THIS COVERS
----------------
`make demo-iso` boots straight into data/wm/demo.script with nobody
touching a key (docs/decisions.md; apps/demo.c). That makes it the one
artifact whose whole job is to run unattended in front of somebody --
and the one nothing tested, because it needs no input and therefore
invites no driver script.

**Ask what a broken version would still pass.** A demo that reached the
desktop and opened four windows still LOOKS like a working tour; the
tour's CLI half can fail every command it runs and the screen still
scrolls past before anyone reads it. That is not hypothetical -- it
shipped: `lscpu` reported "Unknown command" for every demo boot, because
demo_run_cli() runs before apps_start() and the shell's PATH was
initialised inside shell_main(), which a demo boot never reaches. Every
OTHER command in the script (`about`, `df`, `fsck`, `ls`, `lspci`) has
its own builtin dispatch entry and worked perfectly, so the failure was
one line in a scrolling screen.

So the load-bearing check here is #3: a command resolved through PATH
was really EXECUTED. The kernel logs `elf_run: calling
process_run_ring3() for <path>` on the serial port when it happens,
which is a fact to assert on rather than a screen to read.

The checks:

  1. It booted and mounted the live image (not an empty RAM fs).
  2. The tour's CLI half started at all.
  3. A PATH-RESOLVED command ran -- the regression above.
  4. The tour reached the desktop.
  5. The desktop half performed: windows opened, by ring-3 clients.
  6. No panic anywhere in the log.

    python3 tools/demo_test.py
    echo $?

RUN THIS ON DEMAND, NOT ROUTINELY -- standing request from the
maintainer (2026-08-16). It is not in gui_regress.py or preflight.sh and
should not be added to either: it builds and boots its own QEMU from a
separate ISO (the same reason live_boot_test.py is not in them), and the
demo is a showpiece rather than a thing every change can break. Reach
for it when the tour is actually suspect, or after touching
apps/demo.c, data/wm/demo.script, or shell dispatch/init.
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_test import launch_qemu_cmd          # noqa: E402

# The script is ~25s of deliberate `wait` steps in its CLI half plus
# ~15s of desktop steps, and the timings are meant for a person reading
# the screen (data/wm/demo.script). Watch the log until the tour's own
# end marker appears rather than sleeping a fixed interval -- the same
# reasoning as DebugConsole.settle() polling instead of sleeping.
RUN_TIMEOUT_S = 90.0


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def read_log(path):
    if not os.path.exists(path):
        return ""
    with open(path, "r", errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--iso", default="toy-os-demo.iso")
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--qmp-port", type=int, default=4499)
    ap.add_argument("--vnc", type=int, default=9)
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"demo_test: {args.iso} not found -- run `make demo-iso` first")
        return 1

    serial = os.path.abspath(os.path.join(args.tmp, "demo_serial.log"))
    pidfile = os.path.abspath(os.path.join(args.tmp, "demo_qemu.pid"))
    for f in (serial, pidfile):
        if os.path.exists(f):
            os.remove(f)

    # NO DISK: the demo ISO carries its filesystem as a GRUB module, and
    # attaching a disk would let the ordinary disk path run instead --
    # proving nothing about the artifact under test. Same as
    # live_boot_test.py.
    cmd = launch_qemu_cmd(iso=args.iso, disk=None, serial_log=serial,
                          qmp_port=args.qmp_port, vnc_display=args.vnc,
                          pidfile=pidfile)
    subprocess.run(cmd, shell=True, check=True)

    res = Result()
    try:
        # The tour ends by opening Shapes and switching it to the 3D
        # scene; wait for that rather than for a wall-clock guess.
        deadline = time.time() + RUN_TIMEOUT_S
        log = ""
        while time.time() < deadline:
            log = read_log(serial)
            if "gfxdemo: scene 3d" in log or "PANIC:" in log:
                break
            time.sleep(1.0)

        res.check("the demo ISO booted and mounted its live image",
                  "tfs3: mounted" in log and "fs: live image" in log,
                  "no live image mount in the log -- an empty RAM filesystem")

        res.check("the tour's CLI half ran",
                  "elf_run:" in log or "syscall: exit()" in log,
                  "nothing in the CLI half executed anything")

        # THE regression check. /bin/lscpu is the only command in
        # demo.script with no builtin dispatch entry of its own, so it
        # is the only one that can only be found through the shell's
        # PATH -- which makes it the whole tour's canary for shell state
        # that a demo boot never initialised. If this ever needs
        # changing, keep a PATH-only command in the script.
        res.check("a PATH-resolved command really executed",
                  "for /bin/lscpu" in log,
                  "`lscpu` never reached elf_run -- PATH is empty in a "
                  "demo boot (see apps/shell.c's shell_session_init)")

        res.check("the tour reached the desktop",
                  "wm: entering GUI mode" in log,
                  "the CLI half never handed over to the WM")

        # "Windows opened" is asserted as a ring-3 CLIENT opening one,
        # not as the WM drawing something: the desktop is compiled in
        # and would show icons whether or not a single app launched.
        res.check("ring-3 clients opened windows on the desktop",
                  log.count("wm: client pid") >= 2,
                  f"only {log.count('wm: client pid')} client window(s)")

        res.check("no panic", "PANIC:" not in log,
                  "the log contains a kernel panic")

    finally:
        if os.path.exists(pidfile):
            subprocess.run(f"pkill -F {pidfile}", shell=True)

    total = len(res.passes) + len(res.fails)
    print(f"\ndemo_test: {len(res.passes)}/{total} checks passed")
    if res.fails:
        print("  failed: " + ", ".join(res.fails))
        print(f"  full serial log: {serial}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
