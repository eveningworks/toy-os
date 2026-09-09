#!/usr/bin/env python3
"""tools/resize_stride_test.py -- a resized window is composited at the
size it was DRAWN at.

WHAT THIS COVERS
----------------
A window has two buffers, and only the one being drawn into is rebuilt
on a resize (`abi/win_proto.h`'s configure/ack). The compositor has to
know how big the buffer it is about to show actually is, or it walks the
pixels with the wrong row stride and the window shears one pixel per row
-- a diagonal that STAYS until some later resize happens to correct it.

The two ways that has gone wrong are different, and this covers the
second:

* **A second record going stale.** The kernel used to keep each buffer's
  size and a present answered from that copy, which the client had to
  remember to update. It did not, whenever a one-pixel resize left the
  buffer's page count unchanged. Fixed, then designed out: stage 6b put
  the size ON the frame, so there is no second record.
* **The frame carrying the wrong size.** That is what is left, and what
  this checks: `WIN_REQ_PRESENT` names the buffer, its generation and
  its dimensions, and a client that sends any of the three wrongly
  composites at the wrong stride exactly as before.

THE ASSERTION
-------------
Sweep sixteen ONE-PIXEL resizes. After each, the compositor's content
size must equal the size the WM asked for -- it can only know that from
the frame -- and across the sweep the front buffer's GENERATION must
move, which is what says the buffer was really replaced rather than the
numbers merely agreeing.

Not a pixel test, on purpose: the shear is two numbers disagreeing, and
reading them is sharper and cheaper than hunting a diagonal in a PNG.

WHY IT STEPS BY ONE PIXEL
-------------------------
Inherited from the bug that produced this file and still the right
choice. A buffer's length is page rounded, so a larger step crosses a
page boundary and takes a different path through the client's buffer
handling; single-pixel steps are the ones that do not.

POSITIVE CONTROL
----------------
Run with --control for the exact edit. MEASURED: sending `a->w - 1` as
the present's width reddens the sweep on every step.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui     # noqa: E402
from qmp_test import QMPSession                   # noqa: E402
import port_guard                                 # noqa: E402

TITLE = "Terminal"
STEPS = 16

CONTROL = """In userland/ui/uapp.c's present(), lie about the width:

    WIN_PRESENT_SIZE(a->w - 1, a->h)

then `make iso` and re-run. Every sweep step must go red -- the
compositor adopts what the frame says, so the window it reports is one
pixel narrower than the one the client drew.
"""


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def run(dbg, res):
    if not dbg.window(TITLE):
        dbg.open_app(TITLE)
        time.sleep(2.0)
    win = dbg.window(TITLE)
    res.check("Terminal opens as a ring-3 client", win is not None)
    if not win:
        return

    w0, h0 = win["content"]["w"], win["content"]["h"]
    bad, gens = [], set()
    for i in range(STEPS):
        want_w = w0 + i
        dbg.send(f"gui resize {want_w} {h0}")
        time.sleep(1.0)
        dbg.settle()
        win = dbg.window(TITLE)
        if not win:
            bad.append(f"{want_w}: window gone")
            continue
        got = win["content"]["w"]
        # A client may legitimately decline (a fixed-size app, or the
        # screen edge). Terminal does neither at these sizes, so a
        # mismatch here IS the frame carrying a wrong size.
        if got != want_w:
            bad.append(f"asked {want_w}, composited {got}")
        buf = win.get("buf")
        if buf:
            gens.add(buf.get("gen"))

    res.check(f"the composited size matches the drawn size at every step "
              f"({STEPS} 1px resizes)", not bad, "; ".join(bad))

    # THE CONTROL FOR THE CHECK ABOVE. Sizes that agree while the buffer
    # was never replaced would mean the sweep proved nothing about the
    # frame -- the client would be reporting numbers it never drew at.
    res.check("the buffer really was replaced across the sweep",
              len(gens) > 1,
              f"generations seen: {sorted(gens)} -- expected more than one")

    win = dbg.window(TITLE)
    res.check("the sweep actually resized the window",
              win is not None and win["content"]["w"] == w0 + STEPS - 1,
              f"ended at {win['content'] if win else None}, "
              f"wanted w={w0 + STEPS - 1}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--control", action="store_true",
                    help="print the positive control and exit")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "resize_stride_test")

    if args.control:
        print(CONTROL)
        return 0

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, res)
    qmp.close()

    print(f"\nresize_stride_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
