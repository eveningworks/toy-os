#!/usr/bin/env python3
"""The three gesture shapes: hand over a link, move, and not-allowed.

`hand`, `move` and `not-allowed` are cursor-shape-v1's pointer, move and
not-allowed (userland/wm/cursor_theme.h). Each has ONE first caller, and
this drives each of them:

  * hand   -- a link in Help's markdown (uui_markdown's `cursor` op)
  * move   -- a window held by its title bar (the WM's own drag)
  * not-allowed -- a desktop icon dragged over a title bar, which takes
    no drop (wm_dnd_refused_at())

Every check reads the shape the compositor RESOLVED (`gui state --json`
cursor.shape), not pixels -- the pixels are cursor_theme_test.py's job --
and every positive has a control beside it that must NOT show the shape:
prose beside the link, the same title bar after the release, the desktop
and the window's content during the same drag. A shape stuck on, or one
resolved everywhere, fails the control.

Presses land only where no window edge is: a press on a resize zone
starts a resize, and the drag then resizes a window instead of testing
anything (it happened while this was written).

Usage (the VM must already be up):
    python3 tools/vm.py start
    python3 tools/cursor_shapes_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402

D = DebugConsole
checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def close_all(dbg):
    for _ in range(12):
        ws = dbg.json("gui windows --json")["windows"]
        if not ws:
            return
        dbg.send(f"gui close {len(ws) - 1}")
        dbg.settle()


def window(dbg, title, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        for w in dbg.windows():
            if w["title"] == title:
                return w
        time.sleep(0.3)
    return None


def press_at(dbg, qmp, x, y):
    """Park, CONFIRM the WM saw the pointer arrive, then press. A press
    sent straight after the warp can land before the WM's next pass has
    the new position, and starts nothing."""
    D.warp_cursor(dbg, qmp, x, y)
    dbg.settle()
    dbg.cursor()
    qmp.mouse_down()
    time.sleep(0.3)


def drag_to(dbg, qmp, x, y, tries=6):
    """Move the pointer with the button HELD (relative packets carry the
    button), correcting against where the kernel says it got to."""
    for _ in range(tries):
        cx, cy = dbg.cursor()
        dx, dy = x - cx, y - cy
        if abs(dx) <= 2 and abs(dy) <= 2:
            break
        n = max(4, max(abs(dx), abs(dy)) // 12)
        for _ in range(n):
            qmp.move_rel(dx // n, dy // n)
            time.sleep(0.03)
        time.sleep(0.2)
    time.sleep(0.3)
    return dbg.cursor()


def find_link(qmp, w):
    """A link in Help's page: the longest run of the link colour, which
    is an underline. Returns a point on the word above it, or None."""
    path = os.path.join(tempfile.gettempdir(), "cursor_shapes_help.png")
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    c = w["content"]
    x0, x1 = c["x"] + c["w"] // 3, c["x"] + c["w"] - 24   # the page, not the tree
    best = None
    for y in range(c["y"] + 40, c["y"] + c["h"] - 4):
        run = 0
        for x in range(x0, x1):
            r, g, b = im.getpixel((x, y))
            if b > 120 and b - r > 50 and abs(g - (r + b) // 2) < 30:
                run += 1
                if run >= 18 and (best is None or run > best[2]):
                    best = (x - run // 2, y, run)
            else:
                run = 0
    return (best[0], best[1] - 6) if best else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "cursor_shapes_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("cursor gesture shapes")
    close_all(dbg)

    dbg.open_app("Help")
    w = window(dbg, "Help")
    check("Help opens", w is not None)
    if not w:
        return 1
    dbg.settle()

    # --- hand: a link, and the prose beside it ------------------------
    at = find_link(qmp, w)
    check("Help's page shows a link to hover", at is not None, str(at))
    if at:
        D.warp_cursor(dbg, qmp, *at)
        time.sleep(0.6)
        s = dbg.cursor_shape()
        check("a link shows the hand", s == D.CURSOR_HAND, f"shape={s} at {at}")
    c = w["content"]
    prose = (c["x"] + c["w"] - 60, c["y"] + 20)   # the page's top margin
    D.warp_cursor(dbg, qmp, *prose)
    time.sleep(0.6)
    s = dbg.cursor_shape()
    check("control: the page around it does not", s != D.CURSOR_HAND, f"shape={s}")

    # --- move: held by the title bar, and not after -------------------
    tx, ty = w["x"] + w["w"] // 2, w["y"] + 8
    press_at(dbg, qmp, tx, ty)
    time.sleep(0.2)
    drag_to(dbg, qmp, tx + 12, ty + 8)
    s = dbg.cursor_shape()
    check("dragging a window by its title bar shows move", s == D.CURSOR_MOVE, f"shape={s}")
    qmp.mouse_up()
    time.sleep(0.5)
    s = dbg.cursor_shape()
    check("control: the same title bar after the release does not", s != D.CURSOR_MOVE,
          f"shape={s}")
    w = window(dbg, "Help")   # it moved

    # --- not-allowed: an icon over a title bar ------------------------
    icons = dbg.json("gui icons --json")["icons"]
    margin = 12   # past any window's resize zone
    src = None
    for ic in icons:
        px, py = ic["x"] + 6, ic["y"] + ic["h"] // 2
        clear = all(not (ww["x"] - margin <= px < ww["x"] + ww["w"] + margin and
                         ww["y"] - margin <= py < ww["y"] + ww["h"] + margin)
                    for ww in dbg.windows())
        if clear:
            src = (px, py)
            break
    check("a desktop icon is clear of every window", src is not None, str(src))
    # The CONTROL point must be bare desktop as well: a fresh image opens
    # Help at the cascade's first slot, right over src + (30, 10), and
    # the refusal there was the correct answer to the wrong question.
    def bare(px, py):
        return all(not (ww["x"] - margin <= px < ww["x"] + ww["w"] + margin and
                        ww["y"] - margin <= py < ww["y"] + ww["h"] + margin)
                   for ww in dbg.windows())
    desk = None
    if src:
        for ddx, ddy in ((30, 10), (0, 40), (30, 40), (0, 80), (30, 120)):
            if bare(src[0] + ddx, src[1] + ddy) and abs(ddx) + abs(ddy) > 9:
                desk = (src[0] + ddx, src[1] + ddy)
                break
    check("a bare-desktop control point past the drag slop", desk is not None, str(desk))
    if src and desk:
        press_at(dbg, qmp, *src)
        time.sleep(0.2)
        drag_to(dbg, qmp, *desk)   # past the slop: a drag now
        s_desk = dbg.cursor_shape()
        drag_to(dbg, qmp, w["x"] + w["w"] // 2, w["y"] + 8)
        s_title = dbg.cursor_shape()
        drag_to(dbg, qmp, w["x"] + w["w"] // 2, w["y"] + w["h"] // 2)
        s_body = dbg.cursor_shape()
        drag_to(dbg, qmp, *src)   # home again, so the icon keeps its cell
        qmp.mouse_up()
        time.sleep(0.5)
        check("control: the desktop takes the drop, so no refusal there",
              s_desk != D.CURSOR_NOT_ALLOWED, f"shape={s_desk}")
        check("over a title bar the drop is refused: not-allowed",
              s_title == D.CURSOR_NOT_ALLOWED, f"shape={s_title}")
        check("control: a client's content takes it", s_body != D.CURSOR_NOT_ALLOWED,
              f"shape={s_body}")
        s = dbg.cursor_shape()
        check("the refusal ends with the drag", s != D.CURSOR_NOT_ALLOWED, f"shape={s}")

    close_all(dbg)
    failed = sum(1 for _, ok in checks if not ok)
    print(f"\ncursor_shapes_test: {len(checks) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
