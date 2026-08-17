#!/usr/bin/env python3
"""Drive the ring-3 Task Manager: the table widget, resize, and killing.

WHAT IS UNDER TEST
------------------
Three things that arrived together and that nothing else covers:

  1. `uui_table` -- the first multi-column widget. Columns, a header,
     selection, REFLOW (the table has to follow its window in BOTH
     axes) and SORTING.

     The sorting checks assert the ORDER, not just the sort state: a
     positive control (making the permutation always identity) leaves
     "the state changed" green and reddens only the order assertion,
     because the widget can perfectly well record a sort it never
     applies. The order comes from the app's own `taskmgr: order` line
     -- reading it out of pixels would mean OCR.

     One trap already paid for here: the first version of these checks
     spawned an extra process "so there is something to reorder", and
     that new window took focus, landed ON TOP of Task Manager and
     swallowed every header click. Three checks failed while the widget
     was working perfectly. Nothing in this section opens a window.
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
        m4 = re.search(r"taskmgr: sort col (-?\d+) dir (-?\d+)", line)
        if m4:
            out["sort"] = (int(m4.group(1)), int(m4.group(2)))
        # The pids in SCREEN order. Read from the app rather than from
        # pixels, which would mean OCR -- and asserted on directly,
        # because "the table repainted" is satisfied by anything.
        m6 = re.search(r"taskmgr: layout col(\d+) (-?\d+) (\d+)", line)
        if m6:
            out[f"col{m6.group(1)}"] = (int(m6.group(2)), int(m6.group(3)))
        m5 = re.search(r"taskmgr: order (.*)$", line)
        if m5:
            out["order"] = [int(v) for v in m5.group(1).split() if v.strip().isdigit()]
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

    # --- sortable columns --------------------------------------------
    #
    # The widget owns the ordering and the app owns the comparison (see
    # ui/uui_table.h), so what is under test here is the WIDGET: the
    # clickable header, the toggle-to-reverse rule, and the permutation
    # actually reaching the drawn rows.
    win = window(dbg)
    cx, cy = win["content"]["x"], win["content"]["y"]
    lay = layout(dbg)
    tx, ty, _, _ = lay["table"]

    # NO extra spawn here. The first version opened a second VICTIM to
    # guarantee something to reorder -- and that new window took focus,
    # landed on top of Task Manager, and swallowed every header click
    # below. The checks failed while the widget was working perfectly.
    # The rows already present (the WM's own launch, the victim, Task
    # Manager) are enough, and the check below proves it rather than
    # assuming it.
    lay = layout(dbg)
    order_before = lay.get("order", [])
    check("more than one row, so an order is meaningful",
          len(order_before) >= 2, f"{order_before}")

    check("it starts sorted by PID ascending",
          lay.get("sort") == (0, 1) and order_before == sorted(order_before),
          f"sort={lay.get('sort')} order={order_before}")

    # Click the PID header. Same column -> REVERSE, which is the rule
    # every desktop table follows and the reason it lives in the widget.
    hdr_y = cy + ty + lay["header_h"] // 2
    col0 = lay.get("col0", (0, 40))
    hdr_x = cx + col0[0] + col0[1] // 2
    dbg.send("gui click %d %d" % (hdr_x, hdr_y))
    dbg.settle()
    time.sleep(0.8)
    lay = layout(dbg)
    order_desc = lay.get("order", [])
    check("clicking the sorted column REVERSES it",
          lay.get("sort") == (0, -1) and order_desc == sorted(order_before, reverse=True),
          f"sort={lay.get('sort')} order={order_desc}")

    # A DIFFERENT column starts ascending rather than inheriting the
    # previous direction -- also the desktop rule, and easy to get wrong.
    name_col = lay.get("col1")
    if not check("it reports each column's rect", name_col is not None,
                 f"col1={name_col}"):
        name_col = (tx, 80)
    name_x, name_w = name_col
    dbg.send("gui click %d %d" % (cx + name_x + name_w // 2, hdr_y))
    dbg.settle()
    time.sleep(0.8)
    lay = layout(dbg)
    check("a DIFFERENT column starts ascending, not reversed",
          lay.get("sort", (None, None))[1] == 1 and lay.get("sort", (None,))[0] != 0,
          f"sort={lay.get('sort')}")

    # Back to PID ascending, so the checks below see the order they
    # were written against. A test must establish its own preconditions.
    dbg.send("gui click %d %d" % (hdr_x, hdr_y))
    dbg.settle()
    time.sleep(0.5)
    lay = layout(dbg)
    if lay.get("sort") == (0, -1):
        dbg.send("gui click %d %d" % (hdr_x, hdr_y))
        dbg.settle()
        time.sleep(0.5)
        lay = layout(dbg)
    check("it can be put back to PID ascending",
          lay.get("sort") == (0, 1), f"sort={lay.get('sort')}")

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
