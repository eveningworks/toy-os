#!/usr/bin/env python3
"""Drive userland/tests/compclient.c -- Milestone 41 stage 2's raw input
path to a registered ring-3 compositor.

WHAT IS ACTUALLY UNDER TEST
---------------------------
Two things that only mean something together:

  1. A ring-3 process can claim the compositor role (WIN_REQ_SET_
     COMPOSITOR) and receive the input stream the WM consumes, BEFORE
     focus and hit-testing -- WIN_EV_RAW_MOUSE/KEY/WHEEL.
  2. The real WM keeps routing that same input to the same windows while
     it does.

Check 2 is the load-bearing one and it is why UI Demo is opened here at
all. "The compositor received the click" is satisfied by an
implementation that stole the input stream outright, which would be a
regression dressed as a feature -- stage 2 explicitly runs both paths
alongside each other so stage 4 is a deletion rather than a cutover. So
every input this tool injects is asserted TWICE: once in compclient's
log and once in UI Demo's.

The other assertion worth naming is the idle one. The WM loop runs on
every timer tick and the event queue is 32 deep dropping the oldest, so
an implementation that pushed level state unconditionally would report
drops climbing while nobody touched anything. `gui compositor --json`
exposes that count precisely so it can be asserted at zero.

compclient logs to STDERR, which the kernel routes into the kernel log,
so DebugConsole.logs() can read it -- unlike a client's stdout, which
goes to its parent's pipe (see uiclient_test.py's docstring for the trap
in the other direction).

Usage (the VM must already be up and in GUI mode):

    python3 tools/vm.py start
    python3 tools/compositor_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SPAWN_PATH = "/tests/compclient"
UIDEMO_TITLE = "UI Demo"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def comp(dbg):
    """`gui compositor --json` -> dict. The only view of the second
    consumer -- every other `gui` subcommand reports the WM's own state,
    and a compositor is by definition another process."""
    return dbg.json("gui compositor --json")


def comp_lines(dbg, kind=None):
    """compclient's log lines since the last read, optionally one kind.

    logs() clears what it returns, so each call is 'since last time' --
    which is what makes 'this click produced these events' assertable
    rather than 'the log contains a mouse line somewhere'."""
    lines = [l for l in dbg.logs("compclient:") if "compclient:" in l]
    if kind:
        lines = [l for l in lines if f"compclient: {kind} " in l]
    return lines


def parse_mouse(line):
    """'compclient: mouse 300 400 1' -> (300, 400, 1)."""
    tail = line.split("compclient: mouse ", 1)[1].split()
    return int(tail[0]), int(tail[1]), int(tail[2])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    args = ap.parse_args()

    if not args.in_gui:
        qmp = QMPSession(port=args.qmp_port)
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.0)

    dbg = DebugConsole(args.sock)

    print("compositor (M41 stage 2)")

    # --- before anything: no compositor ------------------------------
    # Also the negative half of check 'registers': "pid is nonzero" means
    # nothing unless it was zero a moment earlier.
    before = comp(dbg)
    check("no compositor before spawn", before.get("pid", -1) == 0,
          f"pid={before.get('pid')}")

    # --- a window whose routing must survive -------------------------
    dbg.open_app(UIDEMO_TITLE)
    dbg.settle()
    demo = dbg.window(UIDEMO_TITLE)
    if demo is None:
        check("UI Demo opened", False, "no window -- cannot test coexistence")
        return 1
    check("UI Demo opened", True)

    # Take the button's rect from the app's own `uidemo: layout btn1 x y
    # w h` line rather than re-deriving it here -- a Python copy of a
    # widget offset drifts silently the moment a row is added to the app,
    # which has bitten four tools in this repo already.
    btn = None
    for line in dbg.logs("uidemo:"):     # also drops the startup noise
        if "uidemo: layout btn1 " in line:
            f = line.split("uidemo: layout btn1 ", 1)[1].split()
            btn = tuple(int(v) for v in f[:4])
    if btn is None:
        check("UI Demo reported btn1's layout", False, "no layout line")
        return 1
    check("UI Demo reported btn1's layout", True, f"btn1={btn}")

    # --- register ----------------------------------------------------
    dbg.spawn(SPAWN_PATH)          # no window to wait for -- it has none
    time.sleep(0.5)
    reg = comp_lines(dbg)
    check("client reports registered",
          any("registered" in l for l in reg),
          f"{len(reg)} line(s)")

    after = comp(dbg)
    check("kernel reports a compositor pid", after.get("pid", 0) > 0,
          f"pid={after.get('pid')}")

    # --- idle must not flood -----------------------------------------
    # The WM loop runs every tick. Pushing level state unconditionally
    # would overflow a 32-deep queue in a fraction of a second, so this
    # is the check that a change-gate exists at all.
    dbg.logs()
    time.sleep(1.0)
    idle = comp_lines(dbg, "mouse")
    idle_state = comp(dbg)
    check("idle produces no mouse events", len(idle) == 0, f"{len(idle)} line(s)")
    check("nothing dropped while idle", idle_state.get("dropped", -1) == 0,
          f"dropped={idle_state.get('dropped')}")

    # --- a click reaches BOTH consumers -------------------------------
    content = demo["content"]
    cx = content["x"] + btn[0] + btn[2] // 2
    cy = content["y"] + btn[1] + btn[3] // 2
    dbg.logs()
    dbg.click(cx, cy)
    time.sleep(0.4)

    lines = comp_lines(dbg, "mouse")
    hit = [parse_mouse(l) for l in lines]
    check("compositor saw the click position",
          any(x == cx and y == cy for x, y, _ in hit),
          f"{len(hit)} mouse event(s)")
    check("compositor saw a button press",
          any(b & 1 for _, _, b in hit),
          f"buttons seen: {sorted({b for _, _, b in hit})}")
    # SCREEN coordinates, not window-relative -- that is the whole
    # difference between raw input and the routed WIN_EV_MOUSE_* a client
    # gets. If these came back window-relative they would be small.
    check("coordinates are screen-absolute",
          all(x >= content["x"] for x, _, _ in hit) if hit else False,
          f"content x={content['x']}")

    # --- ...and the WM still routed it -------------------------------
    demo_lines = [l for l in dbg.logs("uidemo:") if "uidemo:" in l]
    check("UI Demo still received the click (both paths live)",
          len(demo_lines) > 0,
          f"{len(demo_lines)} uidemo line(s)")

    # --- a key ---------------------------------------------------------
    dbg.logs()
    dbg.key("a")
    time.sleep(0.4)
    keys = comp_lines(dbg, "key")
    check("compositor saw a keypress", len(keys) > 0, f"{len(keys)} line(s)")

    # --- a wheel notch -------------------------------------------------
    dbg.logs()
    dbg.wheel(-1)
    time.sleep(0.4)
    wheels = comp_lines(dbg, "wheel")
    check("compositor saw a wheel notch", len(wheels) > 0, f"{len(wheels)} line(s)")

    # --- release -------------------------------------------------------
    # 'q' quits compclient. It owns no window, so there is no close
    # button to click and no wm_request_close() to go through -- which is
    # itself the reason the client has a quit key.
    dbg.logs()
    dbg.key("q")
    time.sleep(0.8)
    out = comp_lines(dbg)
    check("client released the role", any("released" in l for l in out),
          f"{len(out)} line(s)")

    gone = comp(dbg)
    check("kernel reports no compositor after release",
          gone.get("pid", -1) == 0, f"pid={gone.get('pid')}")

    # --- and the desktop is unharmed ----------------------------------
    dbg.logs()
    dbg.click(cx, cy)
    time.sleep(0.3)
    still = [l for l in dbg.logs("uidemo:") if "uidemo:" in l]
    check("UI Demo still works after the compositor left", len(still) > 0,
          f"{len(still)} uidemo line(s)")

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    # gui_regress.py pulls the table's summary out of a line matching
    # "passed," and "failed" -- match the other tools' shape or the row
    # falls back to whatever the last line happened to be.
    print(f"\ncompositor_test: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
