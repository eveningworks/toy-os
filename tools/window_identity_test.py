"""tools/window_identity_test.py -- a held window stays the SAME window.

The compositor's windows[] is z-order, and two things renumber it: a
close compacts it and a raise moves one entry to the end. Anything that
holds a window across frames -- a title-bar drag, the window menu, the
shortcut inhibitor -- must still name the window it started with. Each
check below changes the numbering under one of them and asserts that the
ORIGINAL window, and only it, receives what follows:

  * the window menu opened on a lower window survives a close BELOW it:
    its Close closes that window, not the one that slid into its index;
  * the window menu is dismissed when its own window goes;
  * a title-bar drag survives a close below it: `gui state`'s `dragging`
    follows the window to its new index, the release moves THAT window,
    and the one that took the old index does not move;
  * the shortcut inhibitor stays with the window that asked for it: a
    raise of another window puts THAT one in the focused slot, and
    Super+E must fire there.

What a broken version would still pass: none of these -- each asserts a
window by app identity after the numbering changed, so a stale index
reads as the wrong app (menu, drag) or as a shortcut that never fires
(inhibitor).

    python3 tools/window_identity_test.py [--instance N] [--in-gui]
"""
import argparse
import re
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402
from shortcut_test import settings_rects, click_named, fired, SETTINGS_EXEC  # noqa: E402

# By ICON NAME: a client window's `app` is empty and Notepad's title is
# its document's.
APPS = {"Notepad": "notepad", "Calculator": "calculator", "Task Manager": "taskmgr"}


def find(dbg, icon):
    """(z, window) of the first toplevel showing `icon`, or (None, None)."""
    for w in dbg.windows():
        if (w.get("icon") or {}).get("name") == icon and not w.get("popup") \
                and not w.get("dialog"):
            return w["z"], w
    return None, None


def wait_gone(dbg, exe, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if find(dbg, exe)[0] is None:
            return True
        time.sleep(0.25)
    return False


def close_all(dbg):
    for _ in range(12):
        ws = [w for w in dbg.windows() if not w.get("popup") and not w.get("dialog")]
        if not ws:
            return
        dbg.send(f"gui close {ws[-1]['z']}")
        time.sleep(0.8)


def run_menu(dbg, res):
    close_all(dbg)
    for name in ("Notepad", "Calculator", "Task Manager"):
        dbg.open_app(name)
    dbg.settle(1.0)
    _, calc = find(dbg, APPS["Calculator"])
    np_z, _ = find(dbg, APPS["Notepad"])
    if calc is None or np_z is None:
        res.check("the three apps opened", False, f"windows: {dbg.windows()}")
        return
    # A RIGHT-CLICK ON THE TITLE does not raise, so the menu's window
    # stays below Task Manager -- the index a close can shift.
    dbg.rclick(calc["x"] + calc["w"] // 2, calc["y"] + 8)
    time.sleep(0.6)
    res.check("the window menu opened on Calculator",
              dbg.ctxmenu_row("Close") is not None, f"ctxmenu: {dbg.ctxmenu()}")
    dbg.send(f"gui close {np_z}")
    if not wait_gone(dbg, APPS["Notepad"]):
        res.check("Notepad closed below the open menu", False, "it did not close")
        return
    row = dbg.ctxmenu_row("Close")
    res.check("...and the menu is still up after a close BELOW its window",
              row is not None, f"ctxmenu: {dbg.ctxmenu()}")
    if row:
        dbg.click(row[0], row[1])
    calc_gone = wait_gone(dbg, APPS["Calculator"])
    tm = find(dbg, APPS["Task Manager"])[0]
    res.check("its Close closes CALCULATOR, not the window now at its old index",
              calc_gone and tm is not None,
              f"calculator gone={calc_gone}, task manager z={tm}")

    # The menu goes with its window.
    dbg.open_app("Calculator")
    dbg.settle(1.0)
    z, calc = find(dbg, APPS["Calculator"])
    if calc is None:
        res.check("Calculator reopened", False, "no window")
        return
    dbg.rclick(calc["x"] + calc["w"] // 2, calc["y"] + 8)
    time.sleep(0.6)
    dbg.send(f"gui close {z}")
    wait_gone(dbg, APPS["Calculator"])
    time.sleep(0.5)
    m = dbg.ctxmenu()
    res.check("closing the menu's own window dismisses the menu",
              not m.get("open"), f"ctxmenu: {m}")


def run_drag(dbg, qmp, res):
    close_all(dbg)
    for name in ("Task Manager", "Notepad", "Calculator"):
        dbg.open_app(name)
    dbg.settle(1.0)
    tm_z, _ = find(dbg, APPS["Task Manager"])
    calc_z, calc = find(dbg, APPS["Calculator"])
    _, np0 = find(dbg, APPS["Notepad"])
    if None in (tm_z, calc_z, np0):
        res.check("the three apps opened", False, f"windows: {dbg.windows()}")
        return
    gx, gy = calc["x"] + calc["w"] // 3, calc["y"] + 8
    dbg.warp_cursor(qmp, gx, gy)
    qmp.mouse_down()
    time.sleep(0.5)
    st = dbg.state()
    res.check("a held press on Calculator's title starts a drag",
              st.get("dragging") == calc_z, f"dragging={st.get('dragging')}, calc z={calc_z}")

    dbg.send(f"gui close {tm_z}")
    gone = wait_gone(dbg, APPS["Task Manager"])
    calc_z2, _ = find(dbg, APPS["Calculator"])
    st = dbg.state()
    res.check("after a close BELOW it, the drag follows Calculator to its new index",
              gone and st.get("dragging") == calc_z2,
              f"task manager gone={gone}, dragging={st.get('dragging')}, calc z={calc_z2}")

    dbg.warp_cursor(qmp, gx + 80, gy + 60)
    time.sleep(0.4)
    qmp.mouse_up()
    dbg.settle(0.8)
    _, calc2 = find(dbg, APPS["Calculator"])
    _, np2 = find(dbg, APPS["Notepad"])
    moved = calc2 and (calc2["x"] - calc["x"], calc2["y"] - calc["y"])
    res.check("the release moved CALCULATOR",
              moved and abs(moved[0] - 80) <= 8 and abs(moved[1] - 60) <= 8,
              f"calculator moved by {moved}")
    res.check("...and Notepad, which took an index around it, did not move",
              np2 and (np2["x"], np2["y"]) == (np0["x"], np0["y"]),
              f"notepad {np0['x']},{np0['y']} -> "
              f"{np2 and (np2['x'], np2['y'])}")


def run_inhibitor(dbg, res):
    """The inhibitor belongs to the window that asked, not to its slot."""
    close_all(dbg)
    dbg.open_app("Calculator")
    dbg.settle(1.0)
    dbg.send(f"gui spawn {SETTINGS_EXEC}")
    deadline = time.time() + 25
    win = None
    while time.time() < deadline and win is None:
        time.sleep(0.5)
        win = dbg.window("System Settings")
    if win is None:
        res.check("System Settings opened", False, "no window")
        return
    content = win["content"]
    search = None
    for line in dbg.logs("settings: layout search", clear=False):
        m = re.search(r"settings: layout search (-?\d+) (-?\d+) (\d+) (\d+)", line)
        if m:
            search = [int(v) for v in m.groups()]
    if not search:
        res.check("Settings reported its search box", False, "no layout line")
        return
    dbg.send(f"gui click {content['x'] + search[0] + search[2] // 2} "
             f"{content['y'] + search[1] + search[3] // 2}")
    time.sleep(0.5)
    for ch in "shortcuts":
        dbg.send(f"gui key {ord(ch)}")
    time.sleep(2.5)
    dbg.logs("wm: shortcuts", clear=True)
    if not click_named(dbg, res, content, settings_rects(dbg), "screenshot"):
        return
    time.sleep(1.5)
    armed = any("inhibited" in ln for ln in dbg.logs("wm: shortcuts", clear=False))
    res.check("arming a capture in Settings inhibits shortcuts", armed,
              "no 'wm: shortcuts inhibited' line")

    # RAISE THE OTHER WINDOW from its taskbar button: it lands in the
    # focused slot Settings held, which is the slot an index-held
    # inhibitor still names.
    tb = dbg.taskbar()
    buttons = tb.get("buttons") if isinstance(tb, dict) else tb
    b = next((x for x in (buttons or []) if "Calculator" in str(x.get("title", ""))), None)
    if not b:
        res.check("Calculator has a taskbar button", False, f"buttons: {buttons}")
        return
    dbg.send(f"gui click {b['cx']} {b['cy']}")
    dbg.settle(0.8)
    ws = dbg.json("gui windows --json")
    fz = ws.get("focused", -1)
    ftitle = ws["windows"][fz]["title"] if 0 <= fz < len(ws["windows"]) else None
    res.check("the taskbar click focused Calculator",
              ftitle and "Calculator" in ftitle, f"focused: {ftitle}")
    dbg.logs("wm: shortcut ->", clear=True)
    dbg.send("gui key e super")
    got = fired(dbg)
    res.check("Super+E fires over the raised window -- the inhibitor stayed with Settings",
              "/bin/wm/apps/files" in got, f"launched {got}")
    close_all(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "window_identity_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    print("window_identity_test: checks")
    try:
        run_menu(dbg, res)
        run_drag(dbg, qmp, res)
        run_inhibitor(dbg, res)
    finally:
        dbg.close()
        qmp.close()
    print(f"\nwindow_identity_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
