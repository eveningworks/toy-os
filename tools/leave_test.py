#!/usr/bin/env python3
"""The Leave page: Restart / Shut down / Exit to shell, and apps asked to close.

userland/wm/leave_page.c. Start's power rows open it with their own
action focused; choosing an action asks every app window to close and
waits; an app still open after the wait is listed with Cancel or
"<action> anyway".

THE FINAL ACTION IS A DRY RUN (`gui leave dry on`): the page logs
`leave: would ...` instead of powering the machine off, so the tool can
go all the way through every path and still ask questions afterwards.

THE CONTROL IS THE PAIR OF APPS: Calculator closes when asked and must
be reported closed; Notepad with typed text asks to save and must be
reported STILL OPEN. A page that marked everything closed, or nothing,
fails one of the two.

    python3 tools/vm.py start
    python3 tools/leave_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                  # noqa: E402
import port_guard                                # noqa: E402

try:
    from PIL import Image
except ImportError:
    print("leave_test: needs Pillow (pip install pillow)", file=sys.stderr)
    sys.exit(2)

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(("  PASS  " if ok else "  FAIL  ") + name + (f"   {detail}" if detail else ""))
    return ok


def leave(dbg):
    return dbg.json("gui leave --json") or {}


def control(dbg, name):
    return next((c for c in leave(dbg).get("controls", []) if c["name"] == name), None)


def press(dbg, name):
    c = control(dbg, name)
    if c:
        dbg.click(c["cx"], c["cy"])
        dbg.settle()
    return c is not None


def open_from_start(dbg, row):
    st = dbg.json("gui taskbar --json")["start"]
    dbg.click(st["cx"], st["cy"])
    dbg.settle()
    time.sleep(0.5)
    x, y = dbg.menu_row(row)
    dbg.click(x, y)
    dbg.settle()
    time.sleep(1.0)


def wait_log(dbg, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        hit = [line for line in dbg.logs("leave:", clear=False) if needle in line]
        if hit:
            return hit[-1]
        time.sleep(0.3)
    return None


def run(dbg, qmp, tmp):
    for w in sorted(dbg.windows(), key=lambda w: -w["z"]):
        dbg.send(f"gui close {w['z']}")
        time.sleep(0.3)
    dbg.send("gui leave dry on")

    # --- 1. Start's Shutdown row opens the page with Shut down focused --
    before = os.path.join(tmp, "leave_before.png")
    qmp.screenshot(before)
    open_from_start(dbg, "Shutdown")
    lv = leave(dbg)
    check("Start's Shutdown opens the Leave page", lv.get("phase") == "choose", str(lv)[:120])
    check("...with Shut down focused", lv.get("focus") == 1, str(lv.get("focus")))
    names = [c["name"] for c in lv.get("controls", [])]
    check("...offering Restart, Shut down, Exit to shell and Cancel",
          {"restart", "shutdown", "exit", "cancel"} <= set(names), str(names))
    after = os.path.join(tmp, "leave_page.png")
    qmp.screenshot(after)
    a = Image.open(before).convert("RGB")
    b = Image.open(after).convert("RGB")
    # The desktop is DIMMED: a corner away from every control is darker.
    pa, pb = a.getpixel((1100, 560)), b.getpixel((1100, 560))   # the teal, clear of every control
    check("...over the desktop, dimmed", sum(pb) < sum(pa) * 0.6, f"before={pa} after={pb}")
    sd = control(dbg, "shutdown")
    if sd:
        # The focused disc is the accent, in its centre's ring.
        p = b.getpixel((sd["cx"] - sd["w"] // 2 + 12, sd["cy"] - 10))
        check("...the focused action drawn in the accent", p[2] > p[0] + 40, f"px={p}")

    # MODAL: a right-click on the desktop under it opens no menu.
    dbg.rclick(1100, 560)
    dbg.settle()
    m = dbg.ctxmenu() or {}
    check("a right-click under the page opens nothing", not m.get("open"), str(m)[:120])

    # --- 2. Esc is Cancel, and the keys move the focus ---------------
    dbg.key("0x96")   # Right: Exit to shell
    check("Right moves the focus", leave(dbg).get("focus") == 2)
    dbg.key("0x1b")
    check("Esc closes it", leave(dbg).get("phase") == "closed")

    # --- 3. apps asked to close: one does, one cannot -----------------
    dbg.open_app("Notepad")
    dbg.settle()
    qmp.send_text("hello")
    dbg.settle()
    dbg.open_app("Calculator")
    dbg.settle()
    open_from_start(dbg, "Shutdown")
    dbg.logs("leave:", clear=True)
    dbg.key("0x0a")   # Enter: the focused Shut down
    check("Shut down asks the two apps to close",
          bool(wait_log(dbg, "asking 2 app(s) to close", 5)))
    check("...and, one still open, says so after the wait",
          bool(wait_log(dbg, "1 app(s) did not close", 12)))
    lv = leave(dbg)
    apps = {x["title"]: x["gone"] for x in lv.get("apps", [])}
    check("Calculator is reported closed", apps.get("Calculator") is True, str(apps))
    np_ = next((t for t in apps if "untitled" in t), None)
    check("Notepad, with unsaved text, is reported still open", np_ is not None and apps[np_] is False,
          str(apps))
    check("...offering Cancel and Shut down anyway",
          {"cancel", "anyway"} <= {c["name"] for c in lv.get("controls", [])}, str(lv)[:160])
    check("Calculator's window is really gone",
          not any(w["title"] == "Calculator" for w in dbg.windows()))

    # --- 4. Cancel: back to the app, whose question is waiting --------
    press(dbg, "cancel")
    check("Cancel closes the page", leave(dbg).get("phase") == "closed")
    check("...and Notepad is still there to save from",
          any("untitled" in w["title"] for w in dbg.windows()))

    # --- 5. ...anyway acts (dry) -----------------------------------
    dbg.key("0x1b")   # Notepad's own Cancel on its "save changes?" box
    dbg.settle()
    open_from_start(dbg, "Shutdown")
    dbg.logs("leave:", clear=True)
    dbg.key("0x0a")
    wait_log(dbg, "did not close", 12)
    press(dbg, "anyway")
    check("Shut down anyway shuts down (dry run)", bool(wait_log(dbg, "would shut down", 5)))

    # --- 6. Restart into a boot entry --------------------------------
    open_from_start(dbg, "Restart")
    lv = leave(dbg)
    check("Start's Restart opens it with Restart focused", lv.get("focus") == 0, str(lv.get("focus")))
    if control(dbg, "into"):
        press(dbg, "into")
        m = dbg.ctxmenu() or {}
        rows = [r for r in m.get("rows", []) if "previous" in r["label"]]
        check("Restart into lists the boot entries", bool(rows), str(m)[:160])
        if rows:
            dbg.logs("leave:", clear=True)
            dbg.click(rows[0]["cx"], rows[0]["cy"])
            dbg.settle()
            wait_log(dbg, "did not close", 12)
            press(dbg, "anyway")
            said = wait_log(dbg, "would restart into", 5)
            check("...and picking one restarts into it (dry run)",
                  bool(said) and "previous kernel" in said, str(said))
    else:
        # Every image this repo builds has "toy-os (previous kernel)"
        # (tools/install_grub.py): no "Restart into" is a failure, not a
        # skip -- otherwise the one path through the boot choice is
        # never run.
        check("Restart into is offered (the image has a second entry)", False,
              str([c["name"] for c in leave(dbg).get("controls", [])]))
        dbg.key("0x1b")

    for w in dbg.windows():
        if "untitled" in w["title"]:
            dbg.send(f"sh kill {w.get('client_pid', 0)}")

    # --- 7. an app with a MODAL DIALOG open is still asked ------------
    # A batch close that skipped windows behind a dialog asked nobody,
    # never armed its wait, and left the page closing forever with only
    # Cancel. Whatever Notepad answers, the page must move on: everything
    # closed (the dry shutdown), or "did not close" and "...anyway".
    deadline = time.time() + 6
    while time.time() < deadline and any("untitled" in w["title"] for w in dbg.windows()):
        time.sleep(0.3)
    dbg.open_app("Notepad")
    dbg.settle()
    for k in ("0xA4", "0x96", "o"):   # F10, Right, o: Edit > Options...
        dbg.send(f"gui key {k}")
        dbg.settle(0.3)
    deadline = time.time() + 12
    while time.time() < deadline and not any("Notepad Options" in w["title"] for w in dbg.windows()):
        time.sleep(0.3)
    opts = any("Notepad Options" in w["title"] for w in dbg.windows())
    check("Notepad with its Options dialog open", opts, str([w["title"] for w in dbg.windows()]))
    open_from_start(dbg, "Shutdown")
    dbg.logs("leave:", clear=True)
    dbg.key("0x0a")   # Enter: Shut down
    asked = wait_log(dbg, "asking", 5)
    deadline = time.time() + 12
    moved = None
    while time.time() < deadline and not moved:
        moved = wait_log(dbg, "did not close", 0.1) or wait_log(dbg, "would shut down", 0.1)
    lv = leave(dbg)
    check("...Shut down still asks it, and the page moves on",
          bool(asked) and "asking 1 " in asked and bool(moved) and lv.get("phase") != "closing",
          f"asked {asked!r}, then {moved!r}, phase {lv.get('phase')}")
    if lv.get("phase") not in (None, "closed"):
        press(dbg, "cancel")
    for w in dbg.windows():
        if w.get("client_pid") and ("untitled" in w["title"] or "Options" in w["title"]):
            dbg.send(f"sh kill {w['client_pid']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "leave_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        try:
            run(dbg, qmp, args.tmp)
        finally:
            # Never left on: a later tool's Shut down would only log.
            dbg.send("gui leave dry off")
    bad = [n for n, ok in checks if not ok]
    print(f"\nleave_test: {len(checks) - len(bad)} passed, {len(bad)} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
