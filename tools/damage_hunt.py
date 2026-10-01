#!/usr/bin/env python3
"""tools/damage_hunt.py -- run damage_sweep.py over MANY seeds, one VM each.

WHY THIS EXISTS
---------------
`damage_sweep.py --random N --seed S` walks one seeded random ordering
of WM interactions and checks the damage invariant. One seed is one
ordering, and this bug family lives in orderings -- three of the five
damage bugs the sweep has ever found came from the random walk rather
than the fixed sequence. So the useful question is almost never "does
seed 5 pass", it is "does ANY of a batch of seeds fail", and answering
that means a fresh VM per seed (a sweep leaves the desktop full of
windows, and a walk that starts from someone else's mess is not the
walk its seed names).

That loop was written from scratch twice, most recently to re-measure
the recorded 205px violation. It is four lines of shell each time and
gets one thing wrong each time -- usually reusing slot 0 while another
VM is up, or forgetting that `damage_sweep.py`'s `--sock`/`--qmp-port`
have to move together with the slot. Hence a tool.

WHAT IT DOES
------------
For each seed: copy disk.img, start a VM in its own slot (see
`vm.py --instance`), run the sweep against that slot's serial socket
and QMP port, stop the VM, and print one line. Exits non-zero if any
seed reported a violation, so it works as a gate.

    python3 tools/damage_hunt.py                     # seeds 1..8
    python3 tools/damage_hunt.py --seeds 5 11 42     # specific seeds
    python3 tools/damage_hunt.py --random 120 -j 4   # longer walks, parallel

Run it after touching anything that draws, damages, focuses or changes
window chrome, when you want more than the single fixed sequence
`damage_sweep.py` gives you. It is NOT in `gui_regress.py`, for the
same reason `damage_sweep.py` isn't: `gui damage verify on` renders
every frame twice and the walk takes minutes.

-j DEFAULTS TO 1 FOR A REASON
-----------------------------
Parallelism here costs more than CPU. Each slot boots its own guest and
each guest wants its own copy of disk.img, and both of those get worse
faster than the seed count suggests -- at -j 4, four guests booting at
once are slow enough that two of them missed their QMP connect entirely
on the run this paragraph was written from. That is now reported as
ERROR rather than swallowed (see run_seed), but it still means a high
-j buys less than it looks like it does.

What is NOT a reason any more: a `resize-shrink Notepad` violation that
`--seeds 4 6 --random 20 -j 2` was recorded as producing while -j 1 was
clean. It did not reproduce in six attempts -- four at -j 2 idle, and
one each at -j 1 and -j 2 under fourteen busy-looping host cores --
with the harness proven awake by a positive control in between. See
the commit that did it, and note the two harness bugs that measurement
turned up, which are the durable part of it.

A CLEAN RUN STILL PROVES LESS THAN IT LOOKS
-------------------------------------------
It cannot show the absence of damage bugs, only that these orderings
found none. And a clean run is indistinguishable from a harness that
has stopped checking, so before believing one, validate the same way
`damage_sweep.py`'s docstring says to: comment out one
`wm_damage_rect()` call, `make iso`, and confirm violations appear.
(`make iso` -- not `make all`; every headless test boots the ISO.)
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def copy_disk(src, dst):
    """Copy disk.img WITHOUT filling in its holes.

    disk.img is ~4 MB of data inside a 9 GB sparse file, and /tmp is
    commonly a tmpfs -- so `shutil.copyfile` (which writes the zeros out)
    costs 9 GB of RAM per slot. That is not a tidiness point: at -j 4 it
    filled a 32 GB tmpfs outright and the run died of ENOSPC, and at
    -j 2 it put ~18 GB of host memory pressure behind every sweep, which
    is most of what made a parallel run behave unlike a serial one.
    `cp --reflink=auto --sparse=always` keeps the copy sparse (and free,
    on a CoW filesystem); shutil is the fallback for a host without it.
    """
    try:
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, dst],
                       check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        shutil.copyfile(src, dst)


def run_seed(seed, slot, count, keep_logs):
    """One seed on one VM slot.

    Returns (seed, status, violations, summary) where status is "pass",
    "fail" (the invariant was violated) or "error" (the sweep did not
    complete, so it says NOTHING about the invariant either way). The
    third state is the point: this used to report pass/fail only, so a
    sweep that crashed in its first ten seconds landed in the table as
    a PASS -- the exact "green because it tested nothing" failure this
    repo's testing notes warn about most.
    """
    disk = os.path.join(tempfile.gettempdir(), f"damage_hunt_{slot}.img")
    copy_disk(os.path.join(REPO, "disk.img"), disk)

    def vm(*args):
        return subprocess.run(
            [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(slot), *args],
            cwd=REPO, capture_output=True, text=True)

    # vm.py start already waits for the guest to answer on the debug
    # console and exits non-zero if it never does -- this used to throw
    # that away, so a VM that never booted was swept anyway. At -j 4
    # (four guests booting at once) that is not hypothetical: two seeds
    # died in QMPSession's constructor, and both were reported PASS.
    started = vm("--disk", disk, "start")
    if started.returncode != 0:
        vm("stop")
        try:
            os.unlink(disk)
        except OSError:
            pass
        why = (started.stdout + started.stderr).strip().splitlines()
        return seed, "error", [], why[-1] if why else "vm.py start failed"
    try:
        # --sock and --qmp-port must BOTH follow the slot; vm.py derives
        # them from --instance the same way (see its _apply_instance).
        proc = subprocess.run(
            [sys.executable, os.path.join(HERE, "damage_sweep.py"),
             "--sock", f".vm.{slot}.serial" if slot else ".vm.serial",
             "--qmp-port", str(4445 + slot),
             "--random", str(count), "--seed", str(seed)],
            cwd=REPO, capture_output=True, text=True)
    finally:
        vm("stop")
        try:
            os.unlink(disk)
        except OSError:
            pass

    out = proc.stdout + proc.stderr
    if keep_logs:
        with open(os.path.join(keep_logs, f"seed-{seed}.log"), "w") as fh:
            fh.write(out)

    # Match the sweep's OWN per-step report line, not any line mentioning
    # the phrase: `damage_sweep.py` prints "  DAMAGE BUG [label] ...",
    # and repeats each hit in its closing summary. A bare `in` test
    # counted both copies -- and worse, matched a Python TRACEBACK, since
    # gui_debug.py's source contains the string. That is how a run whose
    # sweep died of a BrokenPipeError was reported as a damage violation,
    # quoting a line of Python as the evidence.
    violations = [l.strip() for l in out.splitlines()
                  if l.strip().startswith("DAMAGE BUG [")]
    # Distinct-violation count comes from the tool's own summary line
    # rather than being recounted here -- it dedupes, this shouldn't
    # have to know how. Its ABSENCE means the sweep never finished.
    summary = next((l.strip() for l in out.splitlines() if "distinct violation" in l), None)
    if summary is None or proc.returncode not in (0, 1):
        last = [l.strip() for l in out.splitlines() if l.strip()]
        return seed, "error", violations, (
            f"sweep did not complete (exit {proc.returncode}): "
            f"{last[-1] if last else 'no output'}")
    return seed, ("fail" if violations else "pass"), violations, summary


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", type=int, nargs="+", default=list(range(1, 9)),
                    help="seeds to sweep (default 1..8)")
    ap.add_argument("--random", type=int, default=50, metavar="N",
                    help="randomised interactions per seed, after the fixed sequence (default 50)")
    ap.add_argument("-j", type=int, default=1, metavar="N",
                    help="VMs to run at once (default 1 -- see the docstring's note on "
                         "parallel runs reporting violations a serial run does not)")
    ap.add_argument("--logs", metavar="DIR",
                    help="keep each seed's full output as DIR/seed-N.log")
    args = ap.parse_args()

    if args.logs:
        os.makedirs(args.logs, exist_ok=True)

    print(f"damage_hunt: {len(args.seeds)} seeds x {args.random} random interactions, "
          f"{args.j} at a time")

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as pool:
        futures = {}
        for i, seed in enumerate(args.seeds):
            # Slot is the position in the batch modulo the parallelism,
            # so slots are reused only after their VM has been stopped.
            futures[pool.submit(run_seed, seed, i % args.j, args.random, args.logs)] = seed
        for fut in concurrent.futures.as_completed(futures):
            results.append(fut.result())

    print()
    failed = errored = 0
    for seed, status, violations, summary in sorted(results):
        mark = {"pass": "PASS", "fail": "FAIL", "error": "ERROR"}[status]
        if status == "fail":
            failed += 1
        elif status == "error":
            errored += 1
        print(f"  {mark:<5} seed {seed:<5} {summary}")
        for v in violations:
            print(f"          {v}")

    print()
    if failed:
        print(f"damage_hunt: {failed}/{len(results)} seed(s) violated the damage invariant")
    if errored:
        # Not folded into the failure count: an errored seed did not
        # measure the invariant at all, and calling that a violation
        # would be as wrong as calling it a pass.
        print(f"damage_hunt: {errored}/{len(results)} seed(s) did not complete "
              f"-- they measured NOTHING, clean or otherwise")
    if failed or errored:
        return 1
    print(f"damage_hunt: all {len(results)} seeds clean "
          f"(this proves these orderings found nothing, not that none exist)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
