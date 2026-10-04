"""tools/focus_state_test.py -- clients hear the focus the WM routes by,
and minimize/restore keeps a maximized window maximized.

FOCUS, asserted from the CLIENT's side: uapp logs `uapp: pid N focus F`
whenever a WIN_EV_FOCUS changes what it believes, so each check reads
what the app was told rather than the WM's own `focused` field.

  * minimizing the focused window tells it focus 0 and tells the window
    that inherits the keyboard focus 1 -- the minimize used to tell no
    one, and the window below drew as unfocused while it took the keys;
  * closing the front window above a MINIMIZED one hands focus to the
    visible window under it, and tells the minimized one nothing -- the
    close used to send focus 1 to whatever sat in the slot below.

STATE, asserted from `gui windows`:

  * maximize -> minimize -> restore (taskbar) comes back MAXIMIZED, and
    the window menu's Restore then returns the original rect;
  * the same through the window menu's Minimize and the minimized
    window's Restore row.

What a broken version would still pass: none -- the focus checks name
the pid that must and must not hear an event, and a restore that forgets
maximized reads `normal` with a screen-sized rect.

    python3 tools/focus_state_test.py [--instance N] [--in-gui]
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

FOCUS_RE = re.compile(r"uapp: pid (\d+) focus (\d)")


def find(dbg, icon):
    for w in dbg.windows():
        if (w.get("icon") or {}).get("name") == icon and not w.get("popup") \
                and not w.get("dialog"):
            return w
    return None


def close_all(dbg):
    for _ in range(12):
        ws = [w for w in dbg.windows() if not w.get("popup") and not w.get("dialog")]
        if not ws:
            return
        dbg.send(f"gui close {ws[-1]['z']}")
        time.sleep(0.8)


def open_win(dbg, name, icon):
    dbg.open_app(name)
    dbg.settle(1.0)
    return find(dbg, icon)


def focus_events(dbg, wait=1.5):
    """[(pid, focus)] the clients logged since the last call."""
    time.sleep(wait)
    out = []
    for line in dbg.logs("uapp: pid", clear=True):
        m = FOCUS_RE.search(line)
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


def taskbar_click(dbg, title, right=False):
    tb = dbg.taskbar()
    buttons = tb.get("buttons") if isinstance(tb, dict) else tb
    b = next((x for x in (buttons or []) if x.get("title") == title), None)
    if b:
        dbg.send(f"gui {'rclick' if right else 'click'} {b['cx']} {b['cy']}")
        dbg.settle(1.0)
    return b is not None


def menu_pick(dbg, label):
    time.sleep(0.6)
    row = dbg.ctxmenu_row(label)
    if row:
        dbg.click(row[0], row[1])
        dbg.settle(1.2)
    return row is not None


def run_focus(dbg, res):
    close_all(dbg)
    np = open_win(dbg, "Notepad", "notepad")
    calc = open_win(dbg, "Calculator", "calculator")
    if not np or not calc:
        res.check("Notepad and Calculator opened", False, f"windows: {dbg.windows()}")
        return
    a, b = np["client_pid"], calc["client_pid"]
    focus_events(dbg, wait=0.5)

    taskbar_click(dbg, calc["title"])   # focused -> minimizes it
    ev = focus_events(dbg)
    st = (find(dbg, "calculator") or {}).get("state")
    res.check("a taskbar click minimizes the focused Calculator", st == "minimized",
              f"state {st!r}")
    res.check("...and Notepad, which now takes the keys, is told focus 1",
              (a, 1) in ev, f"events {ev} (notepad {a}, calculator {b})")
    res.check("...and Calculator is told focus 0", (b, 0) in ev, f"events {ev}")

    tm = open_win(dbg, "Task Manager", "taskmgr")
    if not tm:
        res.check("Task Manager opened", False, "no window")
        return
    focus_events(dbg, wait=0.5)
    dbg.send(f"gui close {tm['z']}")
    time.sleep(1.5)
    ev = focus_events(dbg)
    res.check("closing the front window above a MINIMIZED one focuses the visible one",
              (a, 1) in ev, f"events {ev} (notepad {a}, minimized calculator {b})")
    res.check("...and tells the minimized window nothing",
              (b, 1) not in ev, f"events {ev}")


def run_state(dbg, res):
    close_all(dbg)
    np = open_win(dbg, "Notepad", "notepad")
    if not np:
        res.check("Notepad opened", False, "no window")
        return
    rect0 = (np["x"], np["y"], np["w"], np["h"])
    title = np["title"]

    def rclick_title():
        w = find(dbg, "notepad")
        dbg.rclick(w["x"] + w["w"] // 2, w["y"] + 8)

    rclick_title()
    menu_pick(dbg, "Maximize")
    st = (find(dbg, "notepad") or {}).get("state")
    res.check("the window menu maximizes Notepad", st == "maximized", f"state {st!r}")

    taskbar_click(dbg, title)                # minimize
    taskbar_click(dbg, title)                # restore
    w = find(dbg, "notepad") or {}
    res.check("taskbar minimize then restore keeps it MAXIMIZED",
              w.get("state") == "maximized",
              f"state {w.get('state')!r}, rect {w.get('w')}x{w.get('h')}")

    rclick_title()
    menu_pick(dbg, "Restore")
    w = find(dbg, "notepad") or {}
    rect = (w.get("x"), w.get("y"), w.get("w"), w.get("h"))
    res.check("...and the window menu's Restore returns the ORIGINAL rect",
              w.get("state") == "normal" and rect == rect0,
              f"state {w.get('state')!r}, rect {rect} vs {rect0}")

    rclick_title()
    menu_pick(dbg, "Maximize")
    rclick_title()
    menu_pick(dbg, "Minimize")
    taskbar_click(dbg, title, right=True)    # a minimized window's menu
    menu_pick(dbg, "Restore")
    w = find(dbg, "notepad") or {}
    res.check("the window menu's Minimize and Restore keep it MAXIMIZED too",
              w.get("state") == "maximized", f"state {w.get('state')!r}")
    close_all(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "focus_state_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    print("focus_state_test: checks")
    try:
        run_focus(dbg, res)
        run_state(dbg, res)
    finally:
        dbg.close()
        qmp.close()
    print(f"\nfocus_state_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
