#!/usr/bin/env python3
"""tools/popup_test.py -- a menu leaves its window: popup surfaces.

Drives the ring-3 Notepad through `gui` and asks the compositor what it
did (`gui windows --json` lists a popup as its own entry, `popup: true`
with the slot it hangs off). What it proves, and what a broken version
would still pass:

  * A View menu opened on a window too small to hold it is placed
    OUTSIDE the window: the compositor lists a popup whose rect leaves
    the parent's content rect, and the popup's background colour is on
    screen at a point the parent does not own -- a point sampled as NOT
    that colour before the menu opened, so a menu clamped inside the
    window (the old behaviour) fails here and nowhere else.
  * A click on a row that lies beyond the parent's edge COMMITS -- input
    on the popup's own surface reaches the app translated into the
    coordinates its widgets hit-test in.
  * With a menu open, a press on ANOTHER window closes the menu and is
    consumed: that window is not raised, the menu owner keeps focus.
    Wayland's and Win32's rule; a press that fell through would raise it.
  * A press on the desktop dismisses through the compositor (the popup
    entry is gone) AND the client agrees (its menu reports closed) -- the
    WIN_EV_POPUP_DONE round trip.
  * Near the bottom of the work area the menu FLIPS above its title, and
    stays clear of the taskbar -- the compositor's positioner, not the
    widget's, since the widget cannot see the taskbar.

Positive controls, each run once when this was written (2026-09-09):
  * `popup_open()` in ui/uapp.c returning 0 (no surface granted) reddens
    the four surface checks -- "lists the open menu as a popup surface",
    "leaves its window", "background OUTSIDE the parent", "a row outside
    the parent commits" -- and nothing else. The menu still works
    in-window, so a check that stayed green there would have been
    measuring the menu, not the surface.
  * `wm_client_popup_route()` answering 2 for every window reddens the
    three "another window" checks: the menu is not dismissed, the client
    does not agree it closed, and Calculator IS raised.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from menubar_test import (npwin, wait_layout, wait_actions, actions, shot,  # noqa: E402
                          spawn, POPUP_BG, CMD_STATUSBAR)
from harness import Results  # noqa: E402

SMALL_W, SMALL_H = 240, 120   # Notepad's minimum -- and too narrow for its View menu


Result = Results


def popups(dbg):
    return [w for w in dbg.windows() if w.get("popup")]


def wait_popups(dbg, n, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        p = popups(dbg)
        if len(p) == n:
            return p
        time.sleep(0.05)
    return popups(dbg)


def wait_content(dbg, w, h, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        win = npwin(dbg)
        if win and win["content"]["w"] == w and win["content"]["h"] == h:
            return win
        time.sleep(0.05)
    return npwin(dbg)


def rect_of(w):
    return (w["x"], w["y"], w["w"], w["h"])


def run(dbg, qmp, tmp, res):
    win = spawn(dbg, res)
    if not win:
        return
    dbg.settle()

    # --- 1. a window too small for its own menu --------------------------
    dbg.send(f"gui resize {SMALL_W} {SMALL_H}")
    win = wait_content(dbg, SMALL_W, SMALL_H)
    content = win["content"]
    res.check("Notepad accepted a window too small to hold its View menu",
              content["w"] == SMALL_W and content["h"] == SMALL_H, f"content {content}")
    lay = wait_layout(dbg, content, lambda l: l.has("menu.title 2"))
    view = lay.rect("menu.title 2")

    # The control point: right of the parent's frame, on the row the menu
    # will occupy. Sampled BEFORE the menu opens.
    px, py = win["x"] + win["w"] + 8, view[1] + view[3] + 12
    im0 = shot(qmp, tmp, "popup_before.png")
    before = im0.getpixel((px, py))

    dbg.click(view[0] + view[2] // 2, view[1] + view[3] // 2)
    dbg.settle()
    lay2 = wait_layout(dbg, content, lambda l: l.popups() == [0])
    ps = wait_popups(dbg, 1)
    res.check("the compositor lists the open menu as a popup surface of Notepad",
              len(ps) == 1 and ps[0]["client_pid"] == win["client_pid"]
              and ps[0]["parent"] == 0, f"popups: {ps}")
    if not ps:
        # The menu is up IN THE WINDOW (the fallback), so every claim about
        # the surface is false rather than unmeasurable -- say so.
        for name in ("the menu leaves its window on the right",
                     "the popup's background is on screen OUTSIDE the parent window",
                     "clicking a row outside the parent window commits it"):
            res.check(name, False, "no popup surface was granted")
        return
    prect = rect_of(ps[0])
    cx1 = content["x"] + content["w"]
    cy1 = content["y"] + content["h"]
    res.check("the menu leaves its window on the right",
              prect[0] + prect[2] > cx1 + 8 and prect[1] + prect[3] <= cy1,
              f"popup {prect}, parent content ends at ({cx1},{cy1})")
    res.check("the compositor placed it where the client believes it is",
              lay2.popups() == [0] and lay2.rect("menu.popup 0") == prect,
              f"client {lay2.rect('menu.popup 0') if lay2.popups() else None}, compositor {prect}")

    im1 = shot(qmp, tmp, "popup_open.png")
    after = im1.getpixel((px, py))
    res.check("the popup's background is on screen OUTSIDE the parent window",
              before != POPUP_BG and after == POPUP_BG,
              f"at ({px},{py}): before {before}, after {after}")

    # --- 2. a row beyond the parent's edge still commits -----------------
    actions(dbg)
    row = lay2.rect("menu.item 0 6")          # View > Status bar
    rx = win["x"] + win["w"] + 10             # on the row, past the frame
    ry = row[1] + row[3] // 2
    res.check("the test's click point really is outside the parent",
              rx < row[0] + row[2] and rx > win["x"] + win["w"], f"x {rx}, row {row}")
    dbg.click(rx, ry)
    dbg.settle()
    got = wait_actions(dbg)
    res.check("clicking a row outside the parent window commits it",
              got == [CMD_STATUSBAR], f"actions {got} (want [{CMD_STATUSBAR}])")
    wait_layout(dbg, content, lambda l: l.popups() == [])
    res.check("the commit took the surface down", wait_popups(dbg, 0) == [],
              f"popups left: {popups(dbg)}")
    # Put the status bar back so nothing later depends on this toggle.
    dbg.click(view[0] + view[2] // 2, view[1] + view[3] // 2)
    wait_layout(dbg, content, lambda l: l.popups() == [0])
    dbg.click(rx, ry)
    wait_actions(dbg)
    wait_popups(dbg, 0)

    # --- 3. the grab: a press on another window is consumed --------------
    dbg.open_app("Calculator")
    dbg.settle()
    calc = None
    deadline = time.time() + 8
    while time.time() < deadline and not calc:
        calc = dbg.window("Calculator")
        time.sleep(0.1)
    res.check("Calculator opened beside Notepad", calc is not None)
    if calc:
        # Raise Notepad again by its title bar, then open the menu.
        dbg.click(win["x"] + win["w"] // 2, win["y"] + 8)
        dbg.settle()
        dbg.click(view[0] + view[2] // 2, view[1] + view[3] // 2)
        wait_layout(dbg, content, lambda l: l.popups() == [0])
        wait_popups(dbg, 1)
        ws = dbg.windows()
        np_z = [w["z"] for w in ws if w["client_pid"] == win["client_pid"] and not w["popup"]][0]
        calc_z = dbg.window("Calculator")["z"]
        res.check("Notepad is above Calculator with its menu open", np_z > calc_z,
                  f"notepad z {np_z}, calculator z {calc_z}")
        # A point in Calculator's content that Notepad's popup does not cover.
        c = dbg.window("Calculator")["content"]
        tx, ty = c["x"] + c["w"] - 6, c["y"] + c["h"] - 6
        covered = any(p["x"] <= tx < p["x"] + p["w"] and p["y"] <= ty < p["y"] + p["h"]
                      for p in popups(dbg))
        res.check("the press point lands on Calculator, not on the menu", not covered,
                  f"point ({tx},{ty}), popups {popups(dbg)}")
        dbg.click(tx, ty)
        dbg.settle()
        res.check("a press on another window dismisses the menu", wait_popups(dbg, 0) == [],
                  f"popups left: {popups(dbg)}")
        lay3 = wait_layout(dbg, content, lambda l: l.popups() == [])
        res.check("...and the client agrees the menu is closed", lay3.popups() == [],
                  f"client levels {lay3.popups()}")
        ws = dbg.windows()
        np_z2 = [w["z"] for w in ws if w["client_pid"] == win["client_pid"]][0]
        calc_z2 = dbg.window("Calculator")["z"]
        res.check("...and the dismissing press did NOT raise Calculator",
                  np_z2 > calc_z2, f"notepad z {np_z2}, calculator z {calc_z2}")
        dbg.send(f"gui close {dbg.window('Calculator')['z']}")
        dbg.settle()

    # --- 4. the desktop dismisses through the compositor ------------------
    dbg.click(view[0] + view[2] // 2, view[1] + view[3] // 2)
    wait_layout(dbg, content, lambda l: l.popups() == [0])
    wait_popups(dbg, 1)
    scr = dbg.json("gui state --json")["screen"]
    dbg.click(scr["w"] - 30, 30)   # top-right desktop, clear of everything
    dbg.settle()
    res.check("a press on the desktop dismisses the popup surface",
              wait_popups(dbg, 0) == [], f"popups left: {popups(dbg)}")
    lay4 = wait_layout(dbg, content, lambda l: l.popups() == [])
    res.check("...and WIN_EV_POPUP_DONE closed the menu in the client",
              lay4.popups() == [], f"client levels {lay4.popups()}")

    # --- 5. the positioner flips above a title near the taskbar -----------
    st = dbg.json("gui state --json")
    work_bottom = st["screen"]["h"] - st["taskbar_h"]
    win = npwin(dbg)
    tx0, ty0 = win["x"] + win["w"] // 2, win["y"] + 8
    ty1 = work_bottom - 70                   # menu bar ends ~50px above the taskbar
    dbg.send(f"gui drag {tx0} {ty0} {tx0} {ty1}")
    dbg.settle()
    deadline = time.time() + 5
    while time.time() < deadline and abs(npwin(dbg)["y"] - (win["y"] + ty1 - ty0)) > 4:
        time.sleep(0.05)
    win = npwin(dbg)
    content = win["content"]
    lay5 = wait_layout(dbg, content, lambda l: l.has("menu.title 2"))
    view = lay5.rect("menu.title 2")
    dbg.click(view[0] + view[2] // 2, view[1] + view[3] // 2)
    wait_layout(dbg, content, lambda l: l.popups() == [0])
    ps = wait_popups(dbg, 1)
    if ps:
        p = rect_of(ps[0])
        res.check("near the taskbar the menu opens ABOVE its title",
                  p[1] + p[3] <= view[1] and p[1] + p[3] > view[1] - 4,
                  f"popup {p}, title {view}")
        res.check("...and the flipped menu stays clear of the taskbar",
                  p[1] + p[3] <= work_bottom, f"popup bottom {p[1] + p[3]}, work area {work_bottom}")
    else:
        res.check("near the taskbar the menu opens ABOVE its title", False, "no popup listed")
    dbg.click(scr["w"] - 30, 30)
    dbg.settle()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "popup_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, args.tmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\npopup_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
