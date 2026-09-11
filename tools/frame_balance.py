#!/usr/bin/env python3
"""Does a process's teardown balance? -- physical frame accounting.

Spawns a process, lets it exit, and compares the physical allocator's
free-frame count against the baseline. A correct teardown returns
exactly what the process took: the count comes back to where it started.

Two failure directions, and they need opposite fixes:

  * The count goes DOWN and keeps going down -- a LEAK. Frames the
    process took were not returned.
  * The count goes UP -- an OVER-FREE, which is far worse and far
    quieter: teardown handed back frames the process never owned, and
    the allocator will hand them out again while their real owner is
    still using them.

The over-free direction is what this tool was written for.
`vmm_destroy_address_space()` used to free every present user PTE, and
every GUI client maps the kernel's own glyph tables read-only
(WIN_FONT_VADDR) -- so closing a GUI app returned four pages of kernel
.rodata to the allocator. Nothing failed at the time; nothing could,
until those frames were handed out and written to.

THE TRAP THIS ENCODES, and it is why the first measurement of the bug
looked like a clean bill of health: **an over-free shows up ONCE and
then stops.** `pmm_free_frame()` only counts a frame that was marked
used, so the second exit finds the same frames already free and changes
nothing. A run of "cycle 2: +0, cycle 3: +0" is not evidence of health
-- only the FIRST cycle after a fresh boot measures anything, which is
why this tool boots its own VM rather than reusing a running one.

The non-GUI control matters for the same reason: a plain ring-3 program
maps nothing it does not own, so its cycles must be flat. If the
control drifts, the fault is in the harness or in ordinary process
teardown, not in the borrowed-mapping path.

Usage:
    python3 tools/frame_balance.py [--cycles N] [--keep]
"""
import argparse
import re
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole  # noqa: E402

# A one-time cost is not a leak: a first GUI cycle allocates page tables
# and per-process structures that later cycles reuse, charged once on
# the first cycle of a boot and never again -- so the first GUI cycle is
# allowed to differ and later ones are not.
FIRST_CYCLE_SLACK = 8


def free_frames(con):
    out = con.send("sh meminfo")
    m = re.search(r"free:\s*(\d+)", out)
    if not m:
        raise RuntimeError("meminfo gave no free-frame count:\n" + out)
    return int(m.group(1))


def cycle_gui(con):
    """Open a GUI client and let it exit through the close handshake.

    Alt+F4 rather than `gui kill`: a killed process becomes a zombie and
    its address space is not torn down at all, so a kill measures
    nothing about teardown. That mistake cost a run.
    """
    con.send("gui spawn /bin/wm/apps/calculator")
    time.sleep(2.0)
    con.settle()
    con.send("gui key 0xa5 alt")
    time.sleep(2.0)
    con.settle()


def cycle_plain(con):
    con.send("gui spawn /bin/hello")
    time.sleep(1.5)
    con.settle()


def cycle_fork(con):
    """A process that forks twenty children, each exiting, then exits.

    fork() shares every page copy-on-write and raises the frame
    refcount; the children's teardowns must decrement rather than free,
    and the parent's must free what is then private. A miscount in
    either direction shows here as a moved count, and nothing about it
    is GUI-shaped -- it is a control on the refcount itself.
    """
    con.send("gui spawn /tests/fork_test --forks 20")
    time.sleep(2.0)
    con.settle()


def cycle_kill(con):
    """Open a GUI client and KILL it, rather than letting it exit.

    A different teardown path entirely, and it used to free nothing at
    all: scheduler_kill() zombied the process and scheduler_poll() reaped
    it by marking the slot unused, while the address space -- ELF pages,
    stack, heap, window buffer -- was only ever destroyed by a process
    calling sys_exit on ITSELF. ~18 frames a kill, compounding, and Force
    Quit reaches it from the desktop.
    """
    con.send("gui spawn /bin/wm/apps/calculator")
    time.sleep(2.0)
    con.settle()
    out = con.send("gui windows --json")
    for pid in sorted(set(re.findall(r'"client_pid":\s*(\d+)', out))):
        if pid != "0":
            con.send("gui kill %s" % pid)
    time.sleep(1.5)
    con.settle()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--keep", action="store_true", help="leave the VM running")
    args = ap.parse_args()

    subprocess.run(["python3", "tools/vm.py", "stop"], capture_output=True)
    launch = subprocess.run(["python3", "tools/vm.py", "start"], capture_output=True)
    if launch.returncode != 0:
        print(launch.stderr.decode()[-2000:])
        return 2

    failures = []
    try:
        con = DebugConsole(".vm.serial")
        con.send("sh spawn /bin/wm/system/toywm")
        time.sleep(3.0)
        con.settle()

        base = free_frames(con)
        print("fresh boot, desktop up: %d free frames" % base)

        prev = base
        for i in range(args.cycles):
            cycle_plain(con)
            now = free_frames(con)
            delta = now - prev
            print("  control  cycle %d: %d (%+d)" % (i + 1, now, delta))
            if delta != 0:
                failures.append("a non-GUI process's teardown moved the count by %+d" % delta)
            prev = now

        for label, run in (("fork", cycle_fork), ("GUI exit", cycle_gui),
                           ("GUI kill", cycle_kill)):
            first = True
            for i in range(args.cycles):
                run(con)
                now = free_frames(con)
                delta = now - prev
                print("  %-8s cycle %d: %d (%+d)" % (label, i + 1, now, delta))
                # Only the FIRST cycle of a boot can see an over-free
                # (afterwards those frames are already free), and only
                # the first can pay the compositor's one-time costs.
                if first:
                    if delta > 0:
                        failures.append(
                            "OVER-FREE: the first %s teardown returned %d frames "
                            "it did not own" % (label, delta))
                    elif delta < -FIRST_CYCLE_SLACK:
                        failures.append("first %s cycle leaked %d frames" % (label, -delta))
                elif delta != 0:
                    failures.append("%s cycle %d moved the count by %+d"
                                    % (label, i + 1, delta))
                first = False
                prev = now
    finally:
        if not args.keep:
            subprocess.run(["python3", "tools/vm.py", "stop"], capture_output=True)

    if failures:
        print("\nframe_balance: FAIL")
        for f in failures:
            print("  " + f)
        return 1
    print("\nframe_balance: PASS -- every teardown balanced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
