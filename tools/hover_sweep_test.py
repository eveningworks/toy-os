#!/usr/bin/env python3
"""Hover over every named widget in every Start-menu app: nothing may happen.

A HOVER IS NOT A COMMAND. Crash Reports shipped with its table's widget
id equal to its Open command's code, and every row the pointer crossed
opened a Notepad -- thirty-six of them on the bare-metal laptop, until
the process table was full. The toolkit now keeps the two apart (a lone
button commits through `on_action`, and MOTION reaches `on_widget` only
with a button held or for a `UUI_TRACK_HOVER` item), and this sweep is
the class-wide check that it stays that way, whatever an app does with
its ids.

For each app: open it, let it settle, then move the pointer over a grid
of points inside every widget it names (`gui widgets`). Afterwards no new
window may exist and no new process may be running. The app's log is
read too: `uapp: BUG` is the toolkit refusing an app at startup (a
widget id declared twice, a button with no `on_action`), and an app it
refused would otherwise look like one that opened with nothing to hover.

HOVER ONLY, deliberately. A widget report is a name and a rect, so the
sweep cannot tell a list from a button -- and a click on a toolbar or a
menu IS a command, so "a click opened something" is not a failure a
sweep can judge. Clicking every named control would press System
Update's "Restart now" and Task Manager's "Force Quit".

Usage (the VM must already be up):
    python3 tools/vm.py start
    python3 tools/hover_sweep_test.py [--only "Crash Reports"]
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402

GRID = (0.15, 0.5, 0.85)      # across a widget
ROW_STEP = 14                 # down a tall one: about a row apart
MAX_POINTS = 48               # per widget, so a huge canvas stays quick

# Apps the sweep must not open, each with the reason. Empty: System
# Update was here until killing it mid-check stopped wedging the disk
# (group_defer_death() in kernel/proc/sched_exit.c), and the sweep now
# exercises exactly that whenever it closes it.
SKIP = {}

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def points(w):
    """Screen points inside a widget: three columns, and rows ROW_STEP
    apart -- a table's rows are what a hover crosses."""
    x0, y0, ww, hh = w["screen"]["x"], w["screen"]["y"], w["w"], w["h"]
    if ww < 2 or hh < 2:
        return []
    ys = [y0 + int(hh * f) for f in GRID] if hh < ROW_STEP * 4 else \
        list(range(y0 + ROW_STEP // 2, y0 + hh - 1, ROW_STEP))
    out = [(x0 + int(ww * fx), y) for y in ys for fx in GRID]
    return out[:MAX_POINTS]


def live_pids(dbg):
    # NOT the `ps` that answers this: it is spawned per call, and with pids
    # that cycle each one has a new number.
    return {p["pid"] for p in dbg.processes()
            if p["state"] != "zombie" and p["name"] != "ps"}


def wait_for(fn, seconds=8.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        v = fn()
        if v:
            return v
        time.sleep(0.4)
    return None


def give_crash_reports_a_row(dbg):
    """A FIXTURE: Crash Reports on a fresh image has no rows, and an empty
    table has nothing to hover -- the bug this sweep was written for would
    pass it. One ring-3 crash from Crash Test gives it a row."""
    mark = len(dbg.logs(clear=False))
    dbg.send("gui spawn /bin/wm/demos/crashtest")
    win = wait_for(lambda: dbg.window("Crash Test"))
    line = wait_for(lambda: next((ln for ln in dbg.logs(clear=False)[mark:]
                                  if "crashtest: layout ring3" in ln), None))
    g = re.search(r"layout ring3 (\d+) (\d+) (\d+) (\d+)", line or "")
    if not win or not g:
        check("fixture: a crash report exists", False,
              "Crash Test did not report its layout (is the layout log on? --in-gui skips that)")
        if win:
            dbg.send(f"gui kill {win['client_pid']}")   # or the sweep finds it already open
        return
    x, y, w, h = (int(v) for v in g.groups())
    dbg.click(win["content"]["x"] + x + w // 2, win["content"]["y"] + y + h // 2)
    gone = wait_for(lambda: dbg.window("Crash Test") is None)
    check("fixture: a crash report exists", bool(gone))
    time.sleep(1.0)


def sweep(dbg, label):
    print(label)
    before = {w["client_pid"] for w in dbg.windows()}
    dbg.logs()                                    # drop what came before
    dbg.open_app(label)
    win = wait_for(lambda: next((w for w in dbg.windows()
                                 if w["client_pid"] not in before), None))
    bugs = [ln for ln in dbg.logs() if "uapp: BUG" in ln]
    check(f"{label}: the toolkit accepted it", not bugs, bugs[0] if bugs else "")
    if not win:
        check(f"{label}: it opened a window", False, "none appeared")
        return
    pid = win["client_pid"]
    time.sleep(1.5)                               # its own startup children
    dbg.settle()
    base_pids = live_pids(dbg)
    base_wins = len(dbg.windows())

    widgets = dbg.widgets(win["title"])
    moved = 0
    for w in widgets.values():
        for x, y in points(w):
            dbg.move(x, y, settle=False)
            moved += 1
    time.sleep(1.0)
    dbg.settle()

    new_pids = live_pids(dbg) - base_pids
    new_wins = len(dbg.windows()) - base_wins
    check(f"{label}: {moved} hovers over {len(widgets)} widget(s) opened nothing",
          not new_pids and new_wins <= 0,
          f"+{new_wins} window(s), new pid(s) {sorted(new_pids)}" if new_pids or new_wins > 0 else "")

    # Close it, and anything it did open, so the next app starts clean.
    for p in sorted(new_pids) + [pid]:
        dbg.send(f"gui kill {p}")
    wait_for(lambda: all(w["client_pid"] != pid for w in dbg.windows()))
    dbg.settle()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--only", action="append", default=[],
                    help="sweep only this app (repeatable)")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "hover_sweep_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("hover sweep")

    def desktop_up():
        try:
            return "screen" in dbg.json("gui state --json")
        except ValueError:
            return False
    if not wait_for(desktop_up, seconds=60):
        check("the desktop answers", False, "no `gui state` within 60 s")
        return 1

    apps = sorted(dbg.menu_apps())
    check("the Start menu lists apps", bool(apps), f"{len(apps)} app(s)")
    give_crash_reports_a_row(dbg)
    for label in apps:
        if args.only and label not in args.only:
            continue
        if label in SKIP:
            print(f"{label}\n  SKIP  {SKIP[label]}")
            continue
        sweep(dbg, label)

    failed = sum(1 for _, ok in checks if not ok)
    print(f"\nhover_sweep_test: {len(checks) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
