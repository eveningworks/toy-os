#!/usr/bin/env python3
"""tools/idle_cpu.py -- how much CPU does toy-os burn doing NOTHING?

Boots a VM, lets the desktop settle, and measures the HOST CPU time of
the QEMU process over a window in which nothing is sent to the guest.

WHY THE HOST'S CLOCK AND NOT THE GUEST'S
----------------------------------------
The guest cannot see this. `ps` bills whichever process was current at
each timer tick, and on a machine where something is always runnable
that is a measure of who got scheduled, not of work done -- parking the
compositor changed its reported CPU by ZERO while moving its STATE from
`ready` to `block(event)`. The host's view of the emulator is outside
that accounting entirely, so it measures the thing that actually costs.

WHAT IT CANNOT TELL YOU
-----------------------
This is TCG. A large part of the reading is the emulator itself --
translating the guest's 100 Hz timer tick costs host cycles no guest
change can remove -- so the ABSOLUTE number is not a property of toy-os
and should never be quoted as one. What it measures well is a
DIFFERENCE, run twice against the same host with one thing changed:

    python3 tools/predates.py --build "make iso" "python3 tools/idle_cpu.py"

which is how the compositor's wait was measured at 42% of a core before
and 37-38% after (2026-08-27, two pairs).

Do not run it beside anything else -- a second guest or a build competing
for cores lands directly in the number.

    python3 tools/idle_cpu.py                # 30 s window
    python3 tools/idle_cpu.py --window 60    # longer, less noise
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The guest is not idle the moment it boots: init starts the desktop,
# the compositor builds its first frame, the wallpaper decodes. Measuring
# through that would report the startup, not the idle.
SETTLE_S = 15.0


def vm(*args):
    return subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), *args],
                          cwd=ROOT, capture_output=True, text=True)


def cpu_ticks(pid):
    """utime + stime for `pid`, in host clock ticks.

    Fields 14 and 15 of /proc/<pid>/stat, counted from the END of the
    comm field rather than by splitting the whole line -- a process name
    containing a space would shift every index otherwise.
    """
    with open(f"/proc/{pid}/stat") as f:
        rest = f.read().rsplit(") ", 1)[1].split()
    return int(rest[11]) + int(rest[12])   # utime, stime (0-based after state)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--window", type=float, default=30.0,
                    help="seconds to measure over (default 30)")
    ap.add_argument("--settle", type=float, default=SETTLE_S,
                    help="seconds to let the desktop come up first")
    args = ap.parse_args()

    hz = os.sysconf("SC_CLK_TCK")

    vm("stop")
    started = vm("start")
    if started.returncode != 0:
        print("idle_cpu: could not start a VM\n" + started.stderr.strip())
        return 1
    try:
        time.sleep(args.settle)
        pidfile = os.path.join(ROOT, ".vm.pid")
        with open(pidfile) as f:
            pid = int(f.read().strip())

        # Report what the guest thinks it is doing, once, before the
        # window opens -- the STATE is the half of this that a guest CAN
        # answer, and a compositor in `ready` while nothing is happening
        # is the shape of the bug this exists to catch.
        ps = vm("exec", "ps").stdout
        wm_state = next((ln.split(None, 4)[3] for ln in ps.splitlines()
                         if "toywm" in ln), "?")

        before = cpu_ticks(pid)
        time.sleep(args.window)          # nothing is sent to the guest here
        after = cpu_ticks(pid)
    finally:
        vm("stop")

    used_s = (after - before) / hz
    pct = 100.0 * used_s / args.window
    print(f"idle_cpu: {used_s:.2f}s of host CPU over {args.window:.0f}s idle "
          f"= {pct:.0f}% of a core; toywm state={wm_state}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
