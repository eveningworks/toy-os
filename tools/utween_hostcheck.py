#!/usr/bin/env python3
"""Check userland/lib/utween.c -- the desktop's one easing tween -- with
the host gcc, against properties a motion must have rather than against
an oracle (there is no reference easing library to compare with, and
the curve's exact shape is a taste; its INVARIANTS are not).

What it asserts, for a spread of (from, to, duration) triples and for
retargets mid-flight:

  - it starts at `from` and ends EXACTLY at `to` (a scroll that settles
    one pixel short is the visible failure of an integer easing)
  - it is MONOTONIC: never moves back toward `from`
  - it is ease-OUT: the first half of the time covers more than half of
    the distance
  - `active` clears exactly once the end is reached, and never before
  - a retarget mid-flight starts from the value at that instant -- no
    jump -- which is what a second wheel notch relies on
  - a zero-length distance or duration is already there

The tween takes its clock as an argument, which is what lets this run
on the host at all: the code under test never reads a guest clock.

    python3 tools/utween_hostcheck.py
    python3 tools/utween_hostcheck.py --positive-control   # must FAIL

Needs only gcc and the standard library.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# argv: from to dur_ms [retarget_at_ms retarget_to]. Prints the value at
# every millisecond, followed by "A" (active) or "-" (done).
DRIVER = r"""
#include "utween.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int from = atoi(argv[1]), to = atoi(argv[2]);
    unsigned dur = (unsigned)atoi(argv[3]);
    int rt_at = argc > 5 ? atoi(argv[4]) : -1;
    int rt_to = argc > 5 ? atoi(argv[5]) : 0;
    // argv[6], when present, is the curve: 1 = ease-in-out.
    int curve = argc > 6 ? atoi(argv[6]) : 0;
    struct utween tw;
    unsigned long long ms = 1000000ull;
    utween_start_curve(&tw, from, to, dur, 0, (enum utween_curve)curve);
    for (unsigned t = 0; t <= 2 * dur + 5; t++) {  // past a retarget's own end too
        if ((int)t == rt_at) utween_retarget(&tw, rt_to, dur, t * ms);
        int v = utween_value(&tw, t * ms);
        printf("%d %c\n", v, utween_active(&tw) ? 'A' : '-');
    }
    return 0;
}
"""


def build(tmp, poison=False):
    """Compile utween.c + the driver. fixed.h is copied rather than
    reached with -Ikernel/include/api, because that directory also
    holds the toolkit's own string.h, which would shadow the host's."""
    hostcheck.stage(tmp, "kernel/include/api/fixed.h")
    hostcheck.stage(tmp, "userland/lib/utween.h")
    # The control: a LINEAR curve -- still monotonic, still lands, and the
    # ease-out check is what must catch it.
    src = hostcheck.stage(tmp, "userland/lib/utween.c", apply=poison, tool="utween_hostcheck",
                          edits=[("fx_t u3 = fx_mul(fx_mul(u, u), u);\n    return FX_ONE - u3;",
                                  "(void)u;\n    return t;")])
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "utween_host", [src, drv], includes=[tmp],
                             tool="utween_hostcheck")


def run(exe, *args):
    out = subprocess.run([exe] + [str(a) for a in args],
                         capture_output=True, text=True, check=True).stdout
    vals, act = [], []
    for line in out.split("\n"):
        if not line.strip():
            continue
        v, a = line.split()
        vals.append(int(v))
        act.append(a == "A")
    return vals, act


def check_motion(name, vals, act, frm, to, dur, fails, ease_out=True):
    def fail(msg):
        fails.append(f"{name}: {msg}")

    if vals[0] != (frm if dur > 0 else to):
        fail(f"starts at {vals[0]}, not {frm if dur > 0 else to}")
    if vals[dur] != to or vals[-1] != to:
        fail(f"ends at {vals[dur]}/{vals[-1]}, not {to}")
    sign = (to > frm) - (to < frm)
    for i in range(1, len(vals)):
        if sign * (vals[i] - vals[i - 1]) < 0:
            fail(f"moves back at t={i}: {vals[i-1]} -> {vals[i]}")
            break
    if dur >= 4 and abs(to - frm) >= 8:
        half = abs(vals[dur // 2] - frm)
        if ease_out:
            if 2 * half <= abs(to - frm):
                fail(f"not ease-out: half the time covered {half} of {abs(to - frm)}")
        else:
            # EASE-IN-OUT starts slowly, so the first half must cover at
            # most half the distance -- the exact opposite property, and
            # the one that distinguishes the two curves. Symmetry makes
            # it land ON half, so allow a rounding pixel either way.
            if 2 * half > abs(to - frm) + 4:
                fail(f"not ease-in-out: half the time covered {half} "
                     f"of {abs(to - frm)} -- it started fast")
    # active: true strictly before the end, false from the end on
    if any(not a for a in act[:dur]) and dur > 0:
        first = act[:dur].index(False)
        fail(f"went inactive at t={first}, before the end at {dur}")
    if any(act[dur:]):
        fail("still active after the end")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="compile a LINEAR curve; the run must then fail")
    args = ap.parse_args()

    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, poison=args.positive_control)

        triples = [(0, 100, 150), (100, 0, 150), (-40, 360, 200), (5, 5, 150),
                   (0, 1, 150), (0, 1000, 16), (0, 300, 0), (7, -900, 300),
                   (0, 45, 150), (0, 100000, 150)]
        for frm, to, dur in triples:
            vals, act = run(exe, frm, to, dur)
            check_motion(f"{frm}->{to}/{dur}ms", vals, act, frm, to, dur, fails)

        # A retarget mid-flight: continuous at the instant of retarget,
        # and it lands on the NEW destination.
        frm, to, dur, at, rt = 0, 100, 150, 60, 250
        vals, act = run(exe, frm, to, dur, at, rt)
        plain, _ = run(exe, frm, to, dur)
        if vals[at] != plain[at]:
            fails.append(f"retarget jumps: {plain[at]} -> {vals[at]} at t={at}")
        if vals[-1] != rt:
            fails.append(f"retarget ends at {vals[-1]}, not {rt}")
        for i in range(at + 1, len(vals)):
            if vals[i] < vals[i - 1]:
                fails.append(f"retarget moves back at t={i}")
                break
        # ...and a retarget BACK past the start reverses cleanly.
        vals, act = run(exe, 0, 100, 150, 60, -50)
        if vals[-1] != -50:
            fails.append(f"reverse retarget ends at {vals[-1]}, not -50")

        # --- EASE-IN-OUT, the curve travel uses ----------------------
        #
        # Same contract as ease-out for the things a caller relies on --
        # exact endpoints, monotonic, goes inactive once -- and the
        # OPPOSITE first-half property, which is the whole reason it
        # exists: it must start SLOWLY, where ease-out starts at full
        # speed.
        for frm, to, dur in [(0, 100, 200), (100, 0, 200), (0, 1000, 240)]:
            vals, act = run(exe, frm, to, dur, -1, 0, 1)
            check_motion(f"in-out {frm}->{to}/{dur}ms", vals, act, frm, to,
                         dur, fails, ease_out=False)

        # Symmetric about the midpoint: f(1/2) = 1/2 exactly. A seam
        # there is a visible hitch halfway through every animation.
        vals, _ = run(exe, 0, 1000, 200, -1, 0, 1)
        mid = vals[100]
        if abs(mid - 500) > 2:
            fails.append(f"ease-in-out is not symmetric: midpoint {mid}, not ~500")

    total = len(triples) + 4 + 3 + 1
    if args.positive_control:
        if fails:
            print(f"utween_hostcheck: positive control FAILED as it must "
                  f"({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("utween_hostcheck: positive control passed -- the harness "
              "cannot see a linear curve", file=sys.stderr)
        return 1
    for f in fails:
        print("  FAIL  " + f)
    print(f"utween_hostcheck: {total - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
