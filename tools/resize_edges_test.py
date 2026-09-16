#!/usr/bin/env python3
"""Resize a window from all EIGHT edges and corners, not just the grip.

**WHAT A BROKEN VERSION STILL PASSES, and why this asserts geometry
rather than "it resized".** Until 2026-09-16 only the right and bottom
edges started a resize, so a test that dragged an edge and checked the
window changed size would pass on three of eight directions and be
silently blind to the other five. So every case here names the edge it
drags, the dimensions that must CHANGE, and -- the half that actually
catches a wrong anchor -- the ones that must NOT.

A left-edge drag is the case with teeth: x and w both move, and the
RIGHT edge (x + w) has to stay put. Getting the sign wrong, or pinning
the wrong corner, moves the window without resizing it, which looks
almost right in a screenshot.

The cursor is checked in the same pass, because a resize zone the
pointer does not advertise is one nobody finds.

    python3 tools/resize_edges_test.py --instance auto
    python3 tools/resize_edges_test.py --instance auto --positive-control
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import port_guard          # noqa: E402
import qmp_test            # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402

TITLE = "Ring 3 Client"
APP = "/tests/winclient"

# How far to drag, and how much slack an assertion allows. The pointer
# is warped in steps and the WM clamps against the screen, so a drag
# does not land on the exact pixel asked for -- what matters is the
# DIRECTION and that the anchored edge did not move.
DRAG = 60
TOL = 6

# name -> (grab point as a fraction of the window's edge, the expected
# cursor, dx, dy, what must change, what must be anchored)
#
# Fractions rather than pixels: the grab point has to land INSIDE the
# resize strip whatever RESIZE_MARGIN is, and "3 px in from the left
# edge" is a constant that goes stale the moment the margin moves.
CASES = [
    ("right",        ("edge", "e"),  "H",     DRAG,  0,    ["w"],      ["x", "y"]),
    ("left",         ("edge", "w"),  "H",    -DRAG,  0,    ["x", "w"], ["y", "right"]),
    ("bottom",       ("edge", "s"),  "V",     0,     DRAG, ["h"],      ["x", "y"]),
    ("top",          ("edge", "n"),  "V",     0,    -DRAG, ["y", "h"], ["x", "bottom"]),
    ("bottom-right", ("corner", "se"), "DIAG",  DRAG,  DRAG, ["w", "h"], ["x", "y"]),
    ("top-left",     ("corner", "nw"), "DIAG", -DRAG, -DRAG, ["x", "y", "w", "h"],
                                                              ["right", "bottom"]),
    ("top-right",    ("corner", "ne"), "DIAG2", DRAG, -DRAG, ["y", "w", "h"],
                                                              ["x", "bottom"]),
    ("bottom-left",  ("corner", "sw"), "DIAG2", -DRAG, DRAG, ["x", "w", "h"],
                                                              ["y", "right"]),
]


def grab_point(win, kind, where):
    """A point inside the window's resize strip for `where`.

    IN by 2 px, never ON the boundary: the right edge is x + w - 1, and
    a point at x + w is outside the window entirely -- it hit-tests as
    the desktop and the drag does nothing at all.
    """
    x, y, w, h = win["x"], win["y"], win["w"], win["h"]
    inset = 2
    # A corner is grabbed at the corner itself; an edge at its midpoint,
    # which is the one place the corner squares cannot claim.
    cx = x + inset if "w" in where else (x + w - 1 - inset if "e" in where else x + w // 2)
    cy = y + inset if "n" in where else (y + h - 1 - inset if "s" in where else y + h // 2)
    return cx, cy


# Where every case starts from. A CASE MUST NOT INHERIT THE LAST ONE'S
# GEOMETRY: the first run of this test dragged the window to (0, 0) and
# then reported top-left, top-right and bottom-left as broken, because a
# window against the screen corner cannot grow further up or left and
# the clamps correctly refused. Far enough from every edge that a DRAG
# in any direction has room.
HOME_X, HOME_Y = 260, 150
HOME_W, HOME_H = 300, 200


def topmost(con):
    """The TOPMOST window titled TITLE, which is the one the pointer hits.

    DebugConsole.window() returns the LOWEST in z-order, and a second
    copy of the test client is easy to end up with -- a re-run against a
    guest that still has one leaves two. Reading one window's geometry
    while clicking another's produced a full red board with nothing
    wrong in the WM at all.
    """
    matches = [w for w in con.windows() if w["title"] == TITLE]
    return max(matches, key=lambda w: w["z"]) if matches else None


def reset_window(con, qmp):
    """Put the window back at a known size and position.

    The title bar is dragged rather than moved by command because there
    is no "move this window" debug call -- `gui move` moves the POINTER.
    The grab point is the middle of the title bar's HEIGHT, which now
    matters: the bar's top RESIZE_MARGIN rows resize, and only the rows
    below them drag.
    """
    con.send(f"gui resize {HOME_W} {HOME_H}")
    con.settle()
    w = topmost(con)
    if w is None:
        return None
    bar = w["content"]["y"] - w["y"]          # title-bar height, font-derived
    gx, gy = w["x"] + w["w"] // 2, w["y"] + bar // 2
    con.drag_real(qmp, gx, gy,
                  gx + (HOME_X - w["x"]), gy + (HOME_Y - w["y"]), steps=6)
    con.settle()
    return topmost(con)


def edges_of(win):
    return {
        "x": win["x"], "y": win["y"], "w": win["w"], "h": win["h"],
        "right": win["x"] + win["w"], "bottom": win["y"] + win["h"],
    }


def run_case(con, qmp, case, failures, control):
    name, (kind, where), want_cursor, dx, dy, must_change, must_hold = case
    before_win = reset_window(con, qmp)
    if before_win is None:
        failures.append(f"{name}: no window titled {TITLE!r}")
        return
    # The reset has to have WORKED, or the case tests the clamps instead
    # of the edge -- which is exactly how this test first fooled itself.
    if before_win["x"] < DRAG or before_win["y"] < DRAG:
        failures.append(f"{name}: reset left the window at "
                        f"({before_win['x']}, {before_win['y']}), too close to "
                        f"the screen edge to drag {DRAG}px -- fixture, not the WM")
        return
    before = edges_of(before_win)
    gx, gy = grab_point(before_win, kind, where)

    # THE CURSOR IS THE DISCOVERABILITY HALF. Warped and confirmed, not
    # injected: an injected position survives one wm_run() iteration and
    # then the real pointer takes over (CLAUDE.md).
    con.warp_cursor(qmp, gx, gy)
    con.settle()
    shape = con.cursor_shape()
    want = getattr(DebugConsole, "CURSOR_" + want_cursor)
    if shape != want:
        failures.append(f"{name}: cursor is {shape}, expected {want} "
                        f"(CURSOR_{want_cursor}) at ({gx}, {gy})")

    con.drag_real(qmp, gx, gy, gx + dx, gy + dy, steps=8)
    con.settle()
    after_win = topmost(con)
    if after_win is None:
        failures.append(f"{name}: window vanished during the drag")
        return
    after = edges_of(after_win)

    moved = [k for k in must_change if abs(after[k] - before[k]) > TOL]
    if len(moved) != len(must_change):
        stuck = [k for k in must_change if k not in moved]
        failures.append(f"{name}: {stuck} did not move "
                        f"(before={before}, after={after})")

    for k in must_hold:
        if abs(after[k] - before[k]) > TOL:
            failures.append(f"{name}: {k} moved by {after[k] - before[k]} "
                            f"and should be anchored (before={before}, after={after})")

    if control:
        print(f"  {name}: {before} -> {after}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--positive-control", action="store_true",
                    help="print every before/after geometry instead of only "
                         "failures -- how you check the harness can see a "
                         "drag at all before trusting a clean run")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "resize_edges_test")

    qmp = qmp_test.QMPSession(port=args.qmp_port)
    enter_gui(qmp, args.sock)
    con = DebugConsole(args.sock)

    failures = []
    try:
        if not con.spawn(APP, title=TITLE):
            print(f"resize_edges_test: {APP} did not open a window", file=sys.stderr)
            return 1
        for case in CASES:
            run_case(con, qmp, case, failures, args.positive_control)
    finally:
        con.close()

    if failures:
        print(f"resize_edges_test: FAIL -- {len(failures)} problem(s)")
        for f in failures:
            print("  " + f)
        return 1
    print(f"resize_edges_test: PASS -- all {len(CASES)} directions resize "
          f"and show the right cursor")
    return 0


if __name__ == "__main__":
    sys.exit(main())
