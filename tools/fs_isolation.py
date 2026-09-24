"""tools/fs_isolation.py -- does I/O on one path slow the filesystem down on another?

WHY THIS EXISTS
---------------
docs/fslock-design.md splits the filesystem's one lock in stages, and each
stage's claim is about WHO STOPS WAITING FOR WHOM: stage 2 (one lock per
mount) says a call on /tmp no longer waits behind a disk wait on /;
stages 3-4 say the same of two files on ONE volume. latency_under_io.py
measures the compositor, whose reads all land on /, so it cannot see
stage 2 at all. This asks the question directly: time a PROBE on one path
alone, then again while a LOAD hammers another path, and compare.

USE --during TO PICK THE LOAD'S PHASE. A probe started at the load's
first progress line spends its window in diskbench's SEQ-write, which
creates its file; a stage that only frees OVERWRITES (fslock 3b) shows
nothing there and ~200x during `--during RND4K-write`.

The probe is /tests/fslat_bench -- stat() in a loop for a fixed time,
timed CALL BY CALL -- not a throughput benchmark: a lock wait of one disk
operation disappears into the average of thousands of microsecond calls
(the first version of this tool used diskbench as the probe and measured
nothing). The MAX and the p99 are the evidence. It SLEEPS between calls
(--gap-ms): a tight loop holds the lock nearly all the time, so under one
lock it starves the load instead of waiting for it, and reads backwards.

    python3 tools/vm.py --instance 3 --kvm start
    python3 tools/fs_isolation.py --instance 3                  # /tmp vs /var/tmp
    python3 tools/fs_isolation.py --instance 3 --probe-path /etc
                                                                # same volume (stages 3-4)

HOW TO READ THE RESULT
----------------------
The probe's call count, average, p99 and MAX, alone and under load. A
probe that waits behind the load shows it in the max and p99 -- one load
call's worth, milliseconds -- where an isolated one stays at its quiet
figures. Compare two builds measured the same way, never an absolute. Use --kvm: under TCG the two
workloads mostly serialise on the one emulated CPU, which is a different
question.

THE LOAD MUST STILL BE RUNNING when the probe finishes, or the loaded arm
measured a quiet machine and its figures flatter the build. The tool checks
that and says so -- and exits 1 -- rather than printing a clean-looking
number.

EXIT CODE
---------
0 when both arms were collected with the load overlapping the probe, 1
otherwise. No threshold on the ratio: what counts as isolated is the
stage's claim, and the caller compares builds.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard                                        # noqa: E402
from qmp_test import QMPSession                          # noqa: E402
from gui_debug import DebugConsole, enter_gui            # noqa: E402

PROBE_OUT = "/var/tmp/fs_isolation.probe.txt"
LOAD_OUT = "/var/tmp/fs_isolation.load.txt"
FIELDS = ("calls", "avg-us", "p99-us", "max-us")


def probe_result(text):
    """{field: int} from fslat_bench's one `fslat:` line, or None."""
    for line in text.splitlines():
        parts = line.split()
        if parts[:1] == ["fslat:"] and len(parts) >= 9:
            return {parts[i]: int(parts[i + 1]) for i in range(1, 9, 2)}
    return None


def spawn(dbg, cmd, report):
    dbg.send(f"sh rm {report}")
    out = dbg.send(f"gui spawn {cmd} --out {report}")
    if "spawned" not in out:
        print(f"fs_isolation: `{cmd}` did not start: {out.strip()}", file=sys.stderr)
        return False
    return True


def wait_for(dbg, report, needle, timeout):
    """Poll `report` until it contains `needle`; the text, or None on timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = dbg.send(f"sh cat {report}")
        if needle in text:
            return text
        time.sleep(1.0)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--probe-path", default="/tmp",
                    help="what the probe stats (default /tmp, a ramfs mount)")
    ap.add_argument("--load-path", default="/var/tmp/fs_isolation.load",
                    help="the load's scratch file (default on /, the disk)")
    ap.add_argument("--secs", type=int, default=5, help="probe seconds per arm (default 5)")
    ap.add_argument("--gap-ms", type=int, default=1,
                    help="probe sleep between calls (default 1; 0 is a tight loop, "
                         "which under one lock STARVES the load rather than waiting)")
    ap.add_argument("--load-size", type=int, default=256, help="load MiB (default 256)")
    ap.add_argument("--during", default="",
                    help="start the probe when the load reaches this diskbench profile "
                         "(e.g. RND4K-write); default: its first progress line")
    ap.add_argument("--timeout", type=float, default=600.0,
                    help="give up on a workload after this long (default 600)")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fs_isolation")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    alone = loaded = None
    overlapped = False
    try:
        probe = (f"/tests/fslat_bench --path {args.probe_path} --secs {args.secs} "
                 f"--gap-ms {args.gap_ms}")
        # --- ARM 1: the probe alone.
        if not spawn(dbg, probe, PROBE_OUT):
            return 1
        text = wait_for(dbg, PROBE_OUT, "fslat:", args.timeout)
        alone = probe_result(text or "")
        if alone is None:
            print("fs_isolation: the probe did not finish alone", file=sys.stderr)
            return 1

        # --- ARM 2: the load first, then the probe once the load is
        # actually doing I/O -- its first progress line, not a sleep.
        if not spawn(dbg, f"/bin/diskbench --size {args.load_size} --path {args.load_path}",
                     LOAD_OUT):
            return 1
        # WHICH PHASE THE PROBE OVERLAPS DECIDES WHAT IT MEASURES: diskbench
        # starts with SEQ-write, which creates its file (allocating), and a
        # probe started at the first progress line spends its window there.
        want = f"diskbench: progress {args.during}" if args.during else "diskbench: progress"
        if wait_for(dbg, LOAD_OUT, want, args.timeout) is None:
            print("fs_isolation: the load never started", file=sys.stderr)
            return 1
        if not spawn(dbg, probe, PROBE_OUT):
            return 1
        text = wait_for(dbg, PROBE_OUT, "fslat:", args.timeout)
        loaded = probe_result(text or "")
        if loaded is None:
            print("fs_isolation: the probe did not finish under load", file=sys.stderr)
            return 1
        overlapped = "diskbench: done" not in dbg.send(f"sh cat {LOAD_OUT}")
        # Let the load finish, so the next run starts on a quiet machine.
        wait_for(dbg, LOAD_OUT, "diskbench: done", args.timeout)
    finally:
        dbg.send(f"sh rm {PROBE_OUT}")
        dbg.send(f"sh rm {LOAD_OUT}")
        dbg.close()

    print(f"fs_isolation: stat({args.probe_path}) every {args.gap_ms} ms for {args.secs}s, "
          f"load diskbench {args.load_size} MiB on {args.load_path}"
          + (f", during {args.during}" if args.during else ""))
    print(f"  {'':<12}" + "".join(f"{f:>10}" for f in FIELDS))
    for name, r in (("alone", alone), ("under load", loaded)):
        print(f"  {name:<12}" + "".join(f"{r[f]:>10}" for f in FIELDS))
    if not overlapped:
        print("fs_isolation: THE LOAD FINISHED BEFORE THE PROBE -- the loaded arm "
              "measured a quiet machine. Raise --load-size.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
