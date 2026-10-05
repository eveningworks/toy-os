#!/usr/bin/env python3
"""tools/taskbar_menu_test.py -- the taskbar's empty-strip menu and the Start button's.

WHAT THIS IS
------------
A right-click on the taskbar used to open a menu only on a window
button; the empty strip and the Start button opened nothing. Since
2026-10-05 (userland/wm/taskbar_menu.c) the strip offers Task Manager,
the taskbar's own options in place and Taskbar settings, and the Start
button offers the system tools and Leave. This drives both through the
WM's own geometry (`gui taskbar --json`, `gui ctxmenu --json`).

  1. The strip's rows, in order -- and a tray click opens NOTHING, and a
     window button still opens that window's menu (the control: a
     strip handler that swallowed the buttons would pass check 1).
  2. A choice is a ROUND TRIP: Combine buttons > Always writes the
     setting, the taskbar adopts it, and the reopened submenu has moved
     its tick -- then it is put back.
  3. Floating panel toggles the panel both ways.
  4. The Start button's rows, in order, Leave > its three actions.
  5. A tool row launches: Task Manager opens a window.
  6. Leave > Shut down opens the Leave page on Shut down (dry run, so
     nothing powers off), and Esc backs out.

    python3 tools/vm.py start
    python3 tools/taskbar_menu_test.py --instance 0

POSITIVE CONTROLS, run when this was written:
  * taskbar_handle_right_click() returning 0 before the Start check ->
    every menu check goes red (9), while the tray check and the
    window-button control stay green.
  * set_choice() not writing the setting -> checks 2 and 3 go red (3),
    and the menus' rows stay green.

CAVEAT
------
Injected input enters below the PS/2 driver (tools/gui_debug.py), so a
clean run says nothing about the real mouse path.
"""

import argparse
import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from qmp_test import QMPSession                # noqa: E402
from harness import Results                    # noqa: E402
import port_guard                              # noqa: E402

STRIP_ROWS = ["Task Manager", "-", "Combine buttons", "Button position", "Buttons",
              "Floating panel", "-", "Taskbar settings"]
START_ROWS = ["Terminal", "File Manager", "-", "Task Manager", "Device Manager",
              "Log Viewer", "Crash Reports", "Boot Manager", "System Update", "-",
              "System Settings", "-", "Leave"]


def labels(m):
    return [r["label"] for r in m.get("rows", [])]


def row(m, label):
    for r in m.get("rows", []):
        if r["label"] == label:
            return r
    return None


def close_menu(dbg):
    for _ in range(4):
        if not dbg.ctxmenu().get("open"):
            return
        dbg.key("0x1b")


def setting(dbg, name):
    out = dbg.send(f"sh config get {name}")
    lines = [ln.strip() for ln in out.splitlines() if ln.strip() and not ln.startswith("---")]
    return lines[-1] if lines else ""


def empty_strip_point(tb):
    """A point on the strip between the last window button and the tray."""
    right = max([b["x"] + b["w"] for b in tb["buttons"]] + [tb["start"]["x"] + tb["start"]["w"]])
    x = (right + tb["tray_x"]) // 2
    return x, tb["y"] + tb["h"] // 2


def open_sub(dbg, m, label):
    r = row(m, label)
    if not r:
        return {}
    dbg.send(f"gui warp {m['x'] + 20} {r['cy']}")
    dbg.settle()
    return dbg.ctxmenu().get("sub") or {}


def wait_window(dbg, title, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        w = dbg.window(title)
        if w:
            return w
        time.sleep(0.3)
    return None


def close_window(dbg, w):
    dbg.send(f"gui click {w['x'] + w['w'] - 14} {w['y'] + 14}")
    dbg.settle()


def run(dbg, res):
    dbg.send("gui spawn /bin/wm/apps/notepad")
    note = wait_window(dbg, "untitled")
    res.check("a window to give the taskbar a button", note is not None)
    tb = dbg.taskbar()

    # --- 1. the strip ---------------------------------------------------
    sx, sy = empty_strip_point(tb)
    dbg.rclick(sx, sy)
    m = dbg.ctxmenu()
    res.check("a right-click on the empty strip opens its menu",
              labels(m) == STRIP_ROWS, f"rows={labels(m)}")
    close_menu(dbg)

    dbg.rclick(tb["tray_x"] + 4, sy)
    res.check("...and one on the tray opens nothing", not dbg.ctxmenu().get("open"),
              f"menu={labels(dbg.ctxmenu())}")
    close_menu(dbg)

    if tb["buttons"]:
        b = tb["buttons"][0]
        dbg.rclick(b["cx"], b["cy"])
        got = labels(dbg.ctxmenu())
        res.check("...while a window button still opens its window's menu",
                  "Minimize" in got and "Close" in got, f"rows={got}")
        close_menu(dbg)

    # --- 2. a choice, round trip -----------------------------------------
    before = setting(dbg, "desktop.taskbar_combine")
    dbg.rclick(sx, sy)
    sub = open_sub(dbg, dbg.ctxmenu(), "Combine buttons")
    ticked = [r["label"] for r in sub.get("rows", []) if r.get("checked")]
    want_tick = {"always": "Always", "full": "When full", "never": "Never"}.get(before or "full")
    res.check("Combine buttons > ticks what is set now",
              labels(sub) == ["Always", "When full", "Never"] and ticked == [want_tick],
              f"rows={labels(sub)} ticked={ticked} setting={before!r}")
    target = "Never" if want_tick != "Never" else "Always"
    r = row(sub, target)
    if r:
        dbg.send(f"gui click {sub['x'] + 20} {r['cy']}")
        dbg.settle()
    word = {"Always": "always", "Never": "never"}[target]
    adopted = None
    for _ in range(20):
        adopted = dbg.taskbar().get("combine")
        if adopted == word:
            break
        time.sleep(0.2)
    res.check(f"...choosing {target} writes the setting and the taskbar adopts it",
              setting(dbg, "desktop.taskbar_combine") == word and adopted == word,
              f"setting={setting(dbg, 'desktop.taskbar_combine')!r} taskbar={adopted!r}")
    dbg.rclick(sx, sy)
    sub = open_sub(dbg, dbg.ctxmenu(), "Combine buttons")
    ticked = [x["label"] for x in sub.get("rows", []) if x.get("checked")]
    res.check("...and the reopened menu has moved its tick", ticked == [target], f"ticked={ticked}")
    close_menu(dbg)
    dbg.send(f"sh config set desktop.taskbar_combine {before or 'full'}")

    # --- 3. the floating panel toggle -----------------------------------
    was = dbg.taskbar().get("float")
    for step in range(2):
        tb = dbg.taskbar()
        sx, sy = empty_strip_point(tb)
        dbg.rclick(sx, sy)
        m = dbg.ctxmenu()
        r = row(m, "Floating panel")
        if r:
            dbg.send(f"gui click {r['cx']} {r['cy']}")
            dbg.settle()
        now = None
        for _ in range(20):
            now = dbg.taskbar().get("float")
            if now == (not was if step == 0 else was):
                break
            time.sleep(0.2)
        res.check(f"Floating panel toggles the panel ({'on' if not was else 'off'} and back)"
                  if step == 0 else "...and back", now == (not was if step == 0 else was),
                  f"float={now} was={was}")

    # --- 4. the Start button --------------------------------------------
    tb = dbg.taskbar()
    dbg.rclick(tb["start"]["cx"], tb["start"]["cy"])
    m = dbg.ctxmenu()
    res.check("a right-click on the Start button opens its tools menu",
              labels(m) == START_ROWS, f"rows={labels(m)}")
    sub = open_sub(dbg, m, "Leave")
    res.check("...with Leave > Restart, Shut down, Exit to shell",
              labels(sub) == ["Restart", "Shut down", "Exit to shell"], f"sub={labels(sub)}")
    close_menu(dbg)

    # --- 5. a tool row launches -----------------------------------------
    dbg.rclick(tb["start"]["cx"], tb["start"]["cy"])
    r = row(dbg.ctxmenu(), "Task Manager")
    if r:
        dbg.send(f"gui click {r['cx']} {r['cy']}")
    tm = wait_window(dbg, "Task Manager")
    res.check("Task Manager from the Start button opens its window", tm is not None)
    if tm:
        close_window(dbg, tm)

    # --- 6. Leave > Shut down, dry ---------------------------------------
    dbg.send("gui leave dry on")
    dbg.rclick(tb["start"]["cx"], tb["start"]["cy"])
    sub = open_sub(dbg, dbg.ctxmenu(), "Leave")
    r = row(sub, "Shut down")
    if r:
        dbg.send(f"gui click {sub['x'] + 20} {r['cy']}")
        dbg.settle()
    lv = dbg.json("gui leave --json")
    # focus 1 is LEAVE_SHUTDOWN (leave_page.h's enum)
    res.check("Leave > Shut down opens the Leave page on Shut down",
              lv.get("phase") == "choose" and lv.get("focus") == 1,
              f"phase={lv.get('phase')} focus={lv.get('focus')}")
    dbg.key("0x1b")
    dbg.settle()
    after = dbg.json("gui leave --json").get("phase")
    res.check("...and Esc backs out of it", after == "closed", f"phase={after}")
    dbg.send("gui leave dry off")

    if note:
        close_window(dbg, note)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "taskbar_menu_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    print("taskbar_menu_test: checks")
    try:
        run(dbg, res)
    finally:
        dbg.close()
    print(f"\ntaskbar_menu_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
