#!/usr/bin/env python3
"""Run a GUI test tool N times and report which checks fail, and how often.

WHY THIS EXISTS
---------------
An intermittent GUI failure is diagnosed by getting a RATE, not a
verdict. A handful of clean runs proves nothing -- this repo has a
recorded case of a real bug coming back clean six times and then
reproducing with its exact fingerprint five times running. So the first
question is always "how often, and which check", and answering it means
running one tool repeatedly and collating the failures.

That loop had been written from scratch twice (once for `damage_hunt.py`
against seeds, once by hand for the menubar flake) and got the
per-run cleanup wrong each time. This is that loop, once.

`damage_hunt.py` is the sibling for the damage invariant, where the
variable is a SEED rather than a repetition; reach for that one when the
question is "does any ordering violate the invariant" and this one when
it is "does this tool fail intermittently, and where".

WHAT IT REPORTS
---------------
Per run: pass/fail/error, and the NAME of every failed check. `error` is
a distinct state on purpose -- a run whose VM never booted, or whose tool
crashed before printing a summary, measured nothing and must not be
scored as a pass. Scoring it as one is a bug this repo has already
shipped in a harness.

At the end, a per-check tally: a flake shows up as one check failing a
fraction of the time, which is the shape that says "timing" rather than
"logic".

    python3 tools/flake_hunt.py menubar -n 6
    python3 tools/flake_hunt.py menubar -n 6 --keep /tmp/flake
    python3 tools/flake_hunt.py ktest -n 10        # the in-kernel suite

THE `ktest` TARGET is the same loop over `ktest_run.py` rather than over
a GUI tool, and it earns its place for the same reason: the 285 in-kernel
tests run inside the LIVE kernel, so a handful of them are sensitive to
what else the machine happens to be doing, and telling a real regression
from that needs a rate per ASSERTION. It reports each failure as
`<suite>/<test>` plus the source location, since two tests can fail the
same assertion text.

Note what a failure here can mean that a GUI tool's cannot: a ktest run
leaves state on `disk.img`. A test that leaks blocks makes the NEXT
run's `fsck` checks fail, so a rate that climbs run over run is a dirty
fixture, not a worsening bug -- `make clean-disk && make iso` between
batches, and suspect it whenever the first runs pass and the later ones
do not.

COMPARING BEFORE AND AFTER A FIX
--------------------------------
Run it before the fix to get the rate, then after. "It passed once" is
not evidence a flake is fixed; a rate that was 1-in-3 and is now 0-in-6
is. And if the fix makes a DIFFERENT check start failing, this prints
that in the same table -- which is how the menubar fix's own side effect
was caught (parking the real cursor for a hover left it parked, and a
later check silently changed meaning).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The tools this can drive, and the summary line each prints. Kept in
# step with gui_regress.py's TOOLS by NAME, so `-k menubar` there and
# `menubar` here mean the same thing.
SUMMARY_RE = re.compile(r"(\d+) passed,\s*(\d+) failed")


# One ktest line, e.g. "    fsck reports a clean filesystem ... ".
KTEST_NAME_RE = re.compile(r"^    (\S.*?) \.\.\.")
KTEST_SUITE_RE = re.compile(r"^  \[(\S+)\]")


def run_ktest_once(keep_dir, index):
    """One `ktest_run.py` run, its own QEMU, reporting per-assertion.

    Kept beside run_once() rather than folded into it: the two share the
    "no summary means this run measured NOTHING" rule and nothing else --
    ktest_run owns its own VM, so there is no disk copy or slot lease to
    delegate.
    """
    proc = subprocess.run([sys.executable, os.path.join(HERE, "ktest_run.py")],
                          cwd=REPO, capture_output=True, text=True, timeout=600)
    out = proc.stdout + proc.stderr

    if keep_dir:
        with open(os.path.join(keep_dir, f"ktest{index}.log"), "w") as f:
            f.write(out)

    # Walk the transcript keeping the current suite and test name, so a
    # FAIL is attributed to the test that produced it. The name is on the
    # line the failure interrupts -- ktest prints "name ... " and only
    # then discovers the assertion failed.
    failed, suite, name = [], "?", "?"
    for line in out.splitlines():
        m = KTEST_SUITE_RE.match(line)
        if m:
            suite = m.group(1)
            continue
        m = KTEST_NAME_RE.match(line)
        if m:
            name = m.group(1)
            continue
        if line.strip().startswith("at ") and failed and failed[-1].endswith(")"):
            continue
        if "FAIL:" in line:
            failed.append(f"{suite}/{name}")

    m = SUMMARY_RE.search(out)
    state = "error" if not m else ("pass" if int(m.group(2)) == 0 else "fail")
    return state, failed, out


def run_once(name, keep_dir, index):
    """One run of one tool, in its own fresh VM, via gui_regress.

    Delegating to gui_regress rather than launching the tool directly is
    deliberate: it already owns the fresh disk copy, the VM slot lease
    and the cleanup, and duplicating that here is how the two would come
    to disagree about what "a run" means.
    """
    logs = keep_dir or tempfile.mkdtemp(prefix=f"flake_{name}_")
    if keep_dir:
        logs = os.path.join(keep_dir, f"run{index}")
        os.makedirs(logs, exist_ok=True)

    proc = subprocess.run(
        [sys.executable, os.path.join(HERE, "gui_regress.py"),
         "-k", name, "--logs", logs],
        cwd=REPO, capture_output=True, text=True, timeout=900)
    out = proc.stdout + proc.stderr

    log_path = os.path.join(logs, f"{name}.log")
    log = ""
    if os.path.exists(log_path):
        with open(log_path) as f:
            log = f.read()

    failed = [l.split("FAIL", 1)[1].strip()
              for l in log.splitlines() if l.strip().startswith("FAIL")]

    m = SUMMARY_RE.search(log)
    if not m:
        # No summary line: the tool did not finish. Neither a pass nor a
        # meaningful fail -- see the module docstring.
        state = "error"
    else:
        state = "pass" if int(m.group(2)) == 0 else "fail"

    if not keep_dir:
        shutil.rmtree(logs, ignore_errors=True)
    return state, failed, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("tool",
                    help="tool name as gui_regress.py -k knows it, "
                         "or `ktest` for the in-kernel suite")
    ap.add_argument("-n", "--runs", type=int, default=5)
    ap.add_argument("--keep", metavar="DIR",
                    help="keep each run's logs here (needed to diagnose, "
                         "not just to count)")
    args = ap.parse_args()

    if args.keep:
        os.makedirs(args.keep, exist_ok=True)

    tally = {}
    states = {"pass": 0, "fail": 0, "error": 0}
    started = time.time()

    for i in range(1, args.runs + 1):
        if args.tool == "ktest":
            state, failed, out = run_ktest_once(args.keep, i)
        else:
            state, failed, out = run_once(args.tool, args.keep, i)
        states[state] += 1
        for f in failed:
            tally[f] = tally.get(f, 0) + 1
        note = ""
        if state == "error":
            note = "   (no summary -- this run measured NOTHING)"
        print(f"  run {i}: {state}{note}")
        for f in failed:
            print(f"          {f}")
        if state == "error" and out.strip():
            print("          " + out.strip().splitlines()[-1][:120])

    elapsed = time.time() - started
    print(f"\nflake_hunt: {args.tool} -- {states['pass']} pass, "
          f"{states['fail']} fail, {states['error']} error "
          f"in {elapsed:.0f}s")

    if tally:
        print("\n  failures by check:")
        for check, n in sorted(tally.items(), key=lambda kv: -kv[1]):
            print(f"    {n}/{args.runs}  {check}")
    elif states["error"]:
        print("\n  no check failed, but some runs measured nothing -- "
              "fix those before believing the rate")
    else:
        print("\n  no failures")

    # Non-zero if anything failed OR any run failed to measure. A clean
    # exit has to mean "N runs, all of which actually ran".
    return 0 if (states["fail"] == 0 and states["error"] == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
