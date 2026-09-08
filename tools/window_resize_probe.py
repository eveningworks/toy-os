#!/usr/bin/env python3
"""Drag a client window's resize grip repeatedly, printing BOTH views of
the window after each drag.

**WHY BOTH.** A window-protocol bug is usually the compositor and the
window server disagreeing rather than either being wrong alone -- so
this prints `guictl windows` (the compositor's list) beside `lswin` (the
kernel's, per buffer). Reading one of them was how three wrong
hypotheses about a resize bug got written in an afternoon.

**WHY A DRAG RATHER THAN `guictl resize`.** They are not the same path:
`guictl resize` proposes a size directly, while a drag proposes one the
WM computes from the pointer. A resize bug that reproduces only under
dragging is invisible to the simpler call, which is exactly the case
this was written for.

    python3 tools/window_resize_probe.py --instance auto
    python3 tools/window_resize_probe.py --drags 4 --dx 60 --dy 40
"""
import argparse
import sys
import time

import port_guard
import qmp_test
from gui_debug import DebugConsole, enter_gui


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--app", default="/tests/winclient",
                    help="binary to spawn (default: the TWP test client)")
    ap.add_argument("--title", default="Ring 3 Client")
    ap.add_argument("--drags", type=int, default=3)
    ap.add_argument("--dx", type=int, default=40)
    ap.add_argument("--dy", type=int, default=28)
    # THE MODE IS AN AXIS, not a detail: `desktop.resize_mode` decides
    # whether the WM repaints the window live during a drag or draws an
    # outline and proposes once on release, and a resize bug that only
    # appears in one of them is invisible to the other.
    ap.add_argument("--mode", default=None,
                    choices=["live", "outline", "auto"],
                    help="desktop.resize_mode to set before dragging")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "window_resize_probe")

    qmp = qmp_test.QMPSession(port=args.qmp_port)
    enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    if args.mode:
        dbg.send(f"sh config set desktop.resize_mode {args.mode}")
        time.sleep(0.5)
    dbg.spawn(args.app)
    time.sleep(1.5)

    win = dbg.window(args.title)
    if not win:
        print(f"window_resize_probe: no window titled {args.title!r}")
        return 1

    print(f"{'drag':>4}  {'asked':>11}  {'compositor':>11}  {'kernel':>11}  "
          f"{'lag':>5} {'paint':>7}  buffers")
    for n in range(args.drags + 1):
        if n:
            win = dbg.window(args.title)
            if not win:
                print(f"{n:>4}  window gone")
                return 1
            # The GRIP is the bottom-right corner of the FRAME, which is
            # what a person drags. Taken from the WM's own geometry
            # rather than computed here -- a hardcoded offset is the
            # thing that rots when the chrome changes.
            gx = win["x"] + win["w"] - 3
            gy = win["y"] + win["h"] - 3
            qmp.drag(gx, gy, gx + args.dx, gy + args.dy)
            dbg.settle()
            time.sleep(0.6)

        w = dbg.window(args.title)
        comp = f'{w["content"]["w"]}x{w["content"]["h"]}' if w else "gone"
        # MATCHED BY PID, never "the first row". Two windows with the
        # same title -- a previous run's client still open -- would
        # otherwise compare one window's compositor view against
        # another's kernel row and report a disagreement that is the
        # PROBE's, which is exactly the class of bug this tool exists to
        # avoid producing.
        want_pid = w.get("pid") if w else None
        kern, bufs = "?", ""
        for l in dbg.send("sh lswin").splitlines():
            f = l.split()
            if len(f) < 6 or not f[0].isdigit():
                continue
            if want_pid is not None and int(f[0]) != want_pid:
                continue
            kern = f"{f[2]}x{f[3]}"
            bufs = " ".join(f[5:])
            break
        # THE WM'S OWN LAG MEASUREMENT, which is what `auto` mode
        # decides on: it watches how long a client takes to answer a
        # resize and falls back to an outline when that grows. A change
        # to the resize path that costs the client time shows up HERE
        # before it shows up as a failing check.
        st = dbg.state()
        lag = st.get("resize_lag_ms", "?")
        paint = st.get("resize_paint", "?")

        asked = "-" if not n else f"+{args.dx},+{args.dy}"
        flag = "" if comp == kern else "   <-- DISAGREE"
        print(f"{n:>4}  {asked:>11}  {comp:>11}  {kern:>11}  "
              f"{lag:>5} {paint:>7}  {bufs}{flag}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
