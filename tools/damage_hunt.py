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
Parallel VMs have been seen reporting a violation that the same seed
does not reproduce serially -- `--seeds 4 6 --random 20 -j 2` reported
an identical `resize-shrink Notepad` violation on both seeds, while
`-j 1` and `-j 2 --random 0` were clean. Whether that is a real
load-sensitive WM bug or an artifact of the harness sampling a
mid-resize frame under contention is NOT diagnosed (see
docs/roadmap.md's known-issues list). Until it is, treat a violation
that only appears at -j > 1 as unconfirmed, and re-check it at -j 1
before believing it.

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

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def run_seed(seed, slot, count, keep_logs):
    """One seed on one VM slot. Returns (seed, violations, summary_line)."""
    disk = f"/tmp/damage_hunt_{slot}.img"
    shutil.copyfile(os.path.join(REPO, "disk.img"), disk)

    def vm(*args):
        return subprocess.run(
            [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(slot), *args],
            cwd=REPO, capture_output=True, text=True)

    vm("--disk", disk, "start")
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

    violations = [l.strip() for l in out.splitlines() if "DAMAGE BUG" in l]
    # Distinct-violation count comes from the tool's own summary line
    # rather than being recounted here -- it dedupes, this shouldn't
    # have to know how.
    summary = next((l for l in out.splitlines() if "distinct violation" in l), "(no summary)")
    return seed, violations, summary.strip()


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
    failed = 0
    for seed, violations, summary in sorted(results):
        mark = "FAIL" if violations else "PASS"
        if violations:
            failed += 1
        print(f"  {mark}  seed {seed:<5} {summary}")
        for v in violations:
            print(f"          {v}")

    print()
    if failed:
        print(f"damage_hunt: {failed}/{len(results)} seed(s) violated the damage invariant")
        return 1
    print(f"damage_hunt: all {len(results)} seeds clean "
          f"(this proves these orderings found nothing, not that none exist)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
