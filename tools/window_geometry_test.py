#!/usr/bin/env python3
"""tools/window_geometry_test.py -- windows come back where you left them.

WHY THIS EXISTS
---------------
The compositor remembers each application's window position and size
across launches (`userland/wm/wm_geometry.c`, `/etc/windows.conf`). Two
halves can rot independently and neither is visible from a screenshot:
the SAVE, which happens once in close_window(), and the RESTORE, which
has to survive a screen that is no longer the size it was.

The restore half is the one with sharp edges, so most of this drives it
from a HAND-WRITTEN config rather than from a previous run. That is
deliberate: a round-trip test alone would pass if both halves were
broken in the same direction -- save the wrong thing, restore the wrong
thing, get it back. Writing a known geometry and requiring exactly that
geometry cannot be satisfied by a symmetric bug.

WHAT IT ASSERTS
---------------
- a geometry that FITS is restored verbatim, including a window sitting
  over the taskbar (that is a legal placement here, so restoring must
  not "helpfully" move it -- an earlier version clamped to the work
  area and moved y=150 to y=120)
- a geometry far LARGER than the screen is clamped to the work area
- a NEGATIVE position is pulled back on screen, title bar reachable
- a real round trip: move a window, close it, reopen it, same geometry
- an app that has never been opened gets its own default, not another
  app's geometry

    python3 tools/vm.py start
    python3 tools/window_geometry_test.py
    echo $?
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard                                      # noqa: E402

DEFAULT_SOCK = ".vm.serial"
CONF = "/etc/windows.conf"
APP = "System Settings"
APP_ID = "settings"


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def close_all(dbg, timeout=12.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ws = dbg.json("gui windows --json")["windows"]
        if not ws:
            return True
        dbg.send(f"gui close {len(ws) - 1}")
        dbg.settle()
        time.sleep(0.4)
    return False


def open_and_read(dbg, app=APP, timeout=15.0):
    """Open `app` and return its window record once one exists."""
    dbg.open_app(app)
    deadline = time.time() + timeout
    while time.time() < deadline:
        dbg.settle()
        ws = dbg.json("gui windows --json")["windows"]
        if ws:
            return ws[-1]
        time.sleep(0.4)
    return None


def set_conf(dbg, line):
    dbg.send(f"sh write {CONF} {line}")
    time.sleep(0.5)


def run(dbg, qmp, res):
    screen = dbg.json("gui state --json")
    sw = screen.get("screen_w") or 1280
    sh = screen.get("screen_h") or 720
    tb = screen.get("taskbar_h") or 40
    print(f"  (screen {sw}x{sh}, taskbar {tb})")

    close_all(dbg)

    # 1. A geometry that fits comes back EXACTLY -- including a window
    #    whose bottom is under the taskbar, which is a placement the WM
    #    allows on purpose.
    want = (200, 150, 800, 560)
    set_conf(dbg, f"{APP_ID}={want[0]},{want[1]},{want[2]},{want[3]}")
    w = open_and_read(dbg)
    got = (w["x"], w["y"], w["w"], w["h"]) if w else None
    res.check("a geometry that fits is restored verbatim", got == want,
              f"wanted {want}, got {got}")
    close_all(dbg)

    # 2. Larger than the screen: clamped to the work area, not restored
    #    whole. A window wider than the screen has its close button off
    #    the right edge.
    set_conf(dbg, f"{APP_ID}=5000,5000,4000,3000")
    w = open_and_read(dbg)
    # EXACTLY the screen width, not merely "not too big". A default-sized
    # window is also "not too big", so `<=` would pass with the whole
    # feature disabled -- which is what the first version of this check
    # did, and the positive control caught it.
    ok = w and w["w"] == sw and w["h"] <= sh - tb
    res.check("an oversized saved geometry is clamped to the work area", bool(ok),
              f"got {w['w']}x{w['h']}, wanted width exactly {sw} and height <= {sh - tb}"
              if w else "no window")
    onscreen = w and 0 <= w["x"] <= sw - w["w"] and 0 <= w["y"] <= sh - w["h"]
    res.check("...and it lands fully on screen", bool(onscreen),
              f"at {w['x']},{w['y']}" if w else "no window")
    close_all(dbg)

    # 3. A negative position is pulled back, title bar reachable.
    set_conf(dbg, f"{APP_ID}=-400,-300,640,480")
    w = open_and_read(dbg)
    # The SIZE is asserted too, and that is what makes this check mean
    # something: 640x480 can only have come from the file, whereas a
    # non-negative position is true of the default placement as well.
    ok = w and w["x"] >= 0 and w["y"] >= 0 and (w["w"], w["h"]) == (640, 480)
    res.check("a negative saved position is pulled back on screen", bool(ok),
              f"at {w['x']},{w['y']} size {w['w']}x{w['h']}" if w else "no window")
    close_all(dbg)

    # 4. TWO APPS, TWO GEOMETRIES, each getting its own. A restore that
    #    ignored the key would hand both the same rect, and a restore
    #    that did nothing would hand both their defaults -- this fails
    #    on either.
    #
    #    BOTH RECTS MUST FIT THE SCREEN or the clamp fires and the check
    #    measures clamping instead of keying: 300,200,900,600 bottoms out
    #    at 800 on a 720-high screen, came back at y=120, and read as a
    #    failure of something else entirely.
    set_conf(dbg, f"{APP_ID}=300,100,900,600")  # fits 1280x720; see below
    dbg.send("sh append /etc/windows.conf taskmgr=40,60,520,420")
    time.sleep(0.5)
    a = open_and_read(dbg)
    a_geom = (a["x"], a["y"], a["w"], a["h"]) if a else None
    close_all(dbg)
    b = open_and_read(dbg, "Task Manager")
    b_geom = (b["x"], b["y"], b["w"], b["h"]) if b else None
    res.check("each app restores its OWN geometry",
              a_geom == (300, 100, 900, 600) and b_geom == (40, 60, 520, 420),
              f"System Settings {a_geom}, Task Manager {b_geom}")
    close_all(dbg)

    # 5. THE SAVE HALF, exercised on its own. Everything above drives
    #    RESTORE from a written file and would still pass if
    #    close_window() never recorded anything. Here the window is
    #    actually DRAGGED and the file is read back, so the two halves
    #    are covered independently rather than only as a round trip --
    #    which a symmetric bug would satisfy.
    dbg.send(f"sh rm {CONF}")
    time.sleep(0.5)
    w = open_and_read(dbg)
    if not w:
        res.check("a drag is recorded on close", False, "no window opened")
        return
    tb_h = w["content"]["y"] - w["y"]
    grab_x = w["x"] + w["w"] // 2
    grab_y = w["y"] + max(1, tb_h // 2)
    qmp.drag(grab_x, grab_y, grab_x + 90, grab_y + 70)
    dbg.settle()
    time.sleep(0.6)
    moved = dbg.json("gui windows --json")["windows"]
    moved_geom = (moved[-1]["x"], moved[-1]["y"]) if moved else None
    close_all(dbg)
    time.sleep(0.6)
    saved = (dbg.send(f"sh cat {CONF}") or "")
    line = [ln for ln in saved.splitlines() if ln.startswith(APP_ID + "=")]
    ok = False
    if line and moved_geom:
        parts = line[0].split("=", 1)[1].split(",")
        ok = len(parts) == 4 and (int(parts[0]), int(parts[1])) == moved_geom
    res.check("a dragged window's new position is recorded on close", ok,
              f"window ended at {moved_geom}, file says {line}")


def main():
    ap = argparse.ArgumentParser()
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "window_geometry_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()

    print(f"\nwindow_geometry_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
