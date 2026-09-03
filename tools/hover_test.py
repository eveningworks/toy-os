#!/usr/bin/env python3
"""A hover change must REPAINT, not just record damage.

WHAT IS UNDER TEST, and why it is not a screenshot. When the pointer
moves onto a different Start-menu row, the highlight has to follow it.
The obvious test -- warp the cursor and photograph the menu -- CANNOT
SEE THIS BUG: the tray clock forces a full repaint once a second, so any
check that settles before looking finds the highlight correctly placed,
having arrived up to a second late. That is precisely the symptom a
person reports as "laggy", and precisely what a settled screenshot
launders away.

So this counts SCENE REPAINTS (`gui state`'s `scene repaints`, which is
wm_render_frame() calls and not the cheap cursor-only path). A hover
change that repaints nothing is the bug, whatever the screen looks like
a second later.

THE MOVES ARE INJECTED, NOT WARPED, and that is deliberate here even
though CLAUDE.md warns injected input cannot test a hover STATE. This
does not test the state -- it tests whether the change caused a frame --
and an injected move is one console round trip (~10 ms) where a warp is
several hundred. Over eight moves that is ~0.1 clock ticks of noise
against ~8 expected repaints, which is the separation the assertion
needs.

REGRESSION GUARD. This was broken by e960ad3, which moved every
overlay's hover into one table and dropped the `redraw_pending = 1` the
old per-overlay code carried -- while fixing the SAME symptom on the
volume flyout. Damage was recorded; nothing asked for the repaint.

    python3 tools/vm.py start
    python3 tools/hover_test.py
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOCK = os.path.join(REPO, ".vm.serial")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}"
          + (f"   -- {detail}" if detail and not ok else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true",
                    help="the desktop is already up; skip entering it")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)

    def raw(cmd):
        return dbg.send(cmd)

    def repaints():
        m = re.search(r"scene repaints: (\d+)", raw("gui state"))
        return int(m.group(1)) if m else -1

    print("hover_test: a hover change repaints, it does not just damage")

    # The Start button, from the WM's own geometry rather than a guess.
    tb = raw("gui taskbar")
    m = re.search(r"start\s+x=\d+\s+w=\d+\s+centre=\((\d+),(\d+)\)", tb)
    if not m:
        print("hover_test: could not find the start button:", tb[:200])
        return 1
    sx, sy = int(m.group(1)), int(m.group(2))

    raw(f"gui click {sx} {sy}")
    dbg.settle()   # a queued click is consumed on the WM's next frame, not on ours
    state = raw("gui state")
    check("the Start menu opened", "start_menu=1" in state, state[:120])

    rows = [int(c) for c in re.findall(r"centre=(\d+)", raw("gui menu"))]
    check("the menu reports its rows", len(rows) >= 6, f"{len(rows)} rows")
    if len(rows) < 6:
        return 1

    # Eight hover changes, as fast as the console allows.
    before = repaints()
    for cy in rows[:8]:
        raw(f"gui move 60 {cy}")
    after = repaints()
    gained = after - before

    # With the fix each move repaints; with the bug only the tray clock
    # does, and eight console round trips take well under a second.
    check("eight hover changes cause repaints", gained >= 6,
          f"{gained} scene repaints for 8 hover changes "
          f"(the bug gives 0-1, from the clock)")

    raw(f"gui click {sx} {sy}")   # close the menu again
    dbg.settle()

    # THE INVERSE, and it is what stops this passing on a WM that simply
    # repaints on every move. With no overlay open a move changes no
    # hover, so it must take the cursor-only path and repaint NOTHING.
    #
    # It is done with the menu CLOSED rather than "within one row",
    # which was the first version and measured the wrong thing: an
    # injected position overrides the pointer for ONE iteration and then
    # snaps back to where the real mouse is, so each move inside the
    # menu changes the hover TWICE -- onto the row and off the menu --
    # and legitimately repaints. That is the tool's mechanism showing
    # through, not the WM misbehaving.
    state = raw("gui state")
    check("the Start menu closed again", "start_menu=0" in state, state[:120])

    before = repaints()
    for dx in (300, 400, 500, 600, 700, 800, 900, 1000):
        raw(f"gui move {dx} 300")
    gained = repaints() - before
    check("a move over nothing does not repaint the scene", gained <= 1,
          f"{gained} scene repaints for 8 moves with no overlay open")

    passed = sum(1 for _, ok in results if ok)
    print(f"hover_test: {passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
