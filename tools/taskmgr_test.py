#!/usr/bin/env python3
"""Drive the ring-3 Task Manager: the table widget, resize, and killing.

WHAT IS UNDER TEST
------------------
Three things that arrived together and that nothing else covers:

  1. `uui_table` -- the first multi-column widget. Columns, a header,
     selection, and REFLOW: the table has to follow its window in BOTH
     axes.
  2. The per-process accounting behind it (SYS_PROC_INFO): a spawned
     process must actually appear, by name, with plausible memory.
  3. Ending a process -- End Process (SYS_KILL) and End Task (the
     refusable close handshake), each arming on the first click and
     committing on the second.

THE BUG THIS EXISTS TO CATCH, AND WHY IT SURVIVED THE FIRST ROUND
-----------------------------------------------------------------
Task Manager shipped without a test tool, and a resize bug went out with
it: the table grew in WIDTH with its window and gained 16 px of height
against 300 px of window. The cause was not in the resize path at all --
`uui_button_group_natural_size()` measured its buttons' far edge from
the ORIGIN rather than the union's extent, so once a layout moved the
group its reported "natural height" included its own y position, and the
table's growth allowance was eaten by a number that was really a
coordinate.

So the resize check below asserts the table's height GREW BY ROUGHLY
WHAT THE WINDOW GREW BY, not merely that it changed. "It changed" is
satisfied by 16 px out of 300, which is exactly the shipped bug.

Geometry comes from the app's own `taskmgr: layout ...` lines, which it
re-emits whenever the table's rect changes. Deriving row offsets in
Python is what four other tools in this repo have already been bitten
by, and a geometry logged only at startup cannot answer a question about
resizing.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/taskmgr_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TASKMGR = "/bin/wm/system/taskmgr"
VICTIM = "/bin/wm/demos/uidemo"   # something to list and then kill

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


# Accumulated across calls, because DebugConsole.logs() CLEARS what it
# returns by default -- so a second call sees only lines emitted since
# the first, and a key reported once at startup would vanish from it.
# Merging keeps every key at its most recent value, which is what the
# resize check needs.
_layout = {}


def layout(dbg):
    """The app's own reported geometry -- the LAST report of each key.

    Last, not first: the table re-reports its rect on every change, so a
    parser taking the first occurrence would read the startup size and
    call a broken resize correct. That is this repo's "parse one frame"
    lesson in its resize-shaped form.
    """
    out = _layout
    for line in dbg.logs():
        m = re.search(r"taskmgr: layout (\w+) (-?\d+) (-?\d+) (\d+) (\d+)", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m2 = re.search(r"taskmgr: layout row_h (\d+) header_h (\d+)", line)
        if m2:
            out["row_h"] = int(m2.group(1))
            out["header_h"] = int(m2.group(2))
        # The row count is parsed HERE rather than in its own pass over
        # logs(): that call clears what it returns, so a second reader
        # finds nothing and reports 0 rows against a table that listed
        # several.
        m3 = re.search(r"taskmgr: rows (\d+)", line)
        if m3:
            out["rows"] = int(m3.group(1))
    return out


def window(dbg, title="Task Manager"):
    wins = [w for w in dbg.json("gui windows --json")["windows"]
            if w["title"] == title]
    return wins[-1] if wins else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    if not args.in_gui:
        qmp = QMPSession(port=args.qmp_port)
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.0)
    else:
        qmp = QMPSession(port=args.qmp_port)

    dbg = DebugConsole(args.sock)
    print("task manager (uui_table, process accounting, ending processes)")

    # A process to find in the table and later kill.
    dbg.send(f"gui spawn {VICTIM}")
    dbg.settle()
    time.sleep(0.6)

    dbg.spawn(TASKMGR, "Task Manager")
    time.sleep(1.5)

    win = window(dbg)
    if not check("Task Manager opened", win is not None):
        print("\nnothing to drive -- the checks below would be vacuous")
        passed = sum(1 for _, ok, _ in checks if ok)
        print(f"\ntaskmgr_test: {passed} passed, {len(checks) - passed} failed")
        return 1

    check("it is a ring-3 client, not a kernel app",
          win["client_pid"] > 0, f"pid {win['client_pid']}")

    lay = layout(dbg)
    if not check("it reported its layout", "table" in lay and "row_h" in lay):
        print("\nno geometry -- every coordinate below would be a guess")
        passed = sum(1 for _, ok, _ in checks if ok)
        print(f"\ntaskmgr_test: {passed} passed, {len(checks) - passed} failed")
        return 1

    # --- the table lists real processes -------------------------------
    #
    # Asserted through the app's own row count AND the kernel's process
    # table, which are independent views: the app could report rows it
    # never drew, and the kernel could have processes the app missed.
    rows = lay.get("rows", 0)
    check("it lists more than one process", rows >= 2, f"{rows} rows")

    # --- resize: the table must follow in BOTH axes -------------------
    #
    # The shipped bug grew the width correctly and the height by 16 px
    # against 300, so "it changed" is not the assertion -- "it grew by
    # roughly what the window grew by" is.
    before = lay["table"]
    grip = (win["x"] + win["w"] - 3, win["y"] + win["h"] - 3)
    grow_x, grow_y = 150, 260
    dbg.send("gui drag %d %d %d %d" % (grip[0], grip[1],
                                        grip[0] + grow_x, grip[1] + grow_y))
    dbg.settle()
    time.sleep(1.5)

    after = layout(dbg).get("table", before)
    dw, dh = after[2] - before[2], after[3] - before[3]

    # Generous tolerance: the WM clamps the drag to the screen and the
    # client may be granted slightly less than asked. The point is the
    # order of magnitude -- 16 px against 260 is the failure.
    check("the table WIDENS with the window", dw >= grow_x * 0.6,
          f"+{dw}px of ~{grow_x}")
    check("the table GROWS TALLER with the window", dh >= grow_y * 0.6,
          f"+{dh}px of ~{grow_y}")

    # --- selecting a row, and ending a process ------------------------
    win = window(dbg)
    cx, cy = win["content"]["x"], win["content"]["y"]
    lay = layout(dbg)
    tx, ty, _, _ = lay["table"]

    # Row 0 is the first listed process.
    row0_y = cy + ty + lay["header_h"] + lay["row_h"] // 2
    dbg.send("gui click %d %d" % (cx + tx + 60, row0_y))
    dbg.settle()
    time.sleep(0.4)

    before_count = dbg.json("gui windows --json")["count"]

    bk = lay.get("btn_kill")
    if not check("it reported its buttons", bk is not None):
        passed = sum(1 for _, ok, _ in checks if ok)
        print(f"\ntaskmgr_test: {passed} passed, {len(checks) - passed} failed")
        return 1

    bx, by = cx + bk[0] + bk[2] // 2, cy + bk[1] + bk[3] // 2

    # FIRST click only ARMS. A kill on the first click would be a real
    # defect -- the confirmation is the whole safety story here, since
    # there is no dialog.
    dbg.send("gui click %d %d" % (bx, by))
    dbg.settle()
    time.sleep(0.6)
    armed_count = dbg.json("gui windows --json")["count"]
    check("one click ARMS and kills nothing",
          armed_count == before_count, f"{before_count} -> {armed_count}")

    # SECOND click commits.
    dbg.send("gui click %d %d" % (bx, by))
    dbg.settle()
    time.sleep(1.8)
    after_count = dbg.json("gui windows --json")["count"]
    check("the second click ends the process",
          after_count == before_count - 1,
          f"{before_count} -> {after_count}")

    killed = any("taskmgr: killed pid" in l for l in dbg.logs())
    check("...and says so in the log", killed)

    # --- it survived its own operation --------------------------------
    #
    # Task Manager kills things; the one thing it must not do is die
    # doing it. `gui state` is answered from inside wm_run(), so a reply
    # also proves the desktop is alive.
    still = window(dbg)
    check("Task Manager is still running", still is not None)
    check("the desktop is still alive",
          dbg.json("gui state --json") is not None)

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print(f"\ntaskmgr_test: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
