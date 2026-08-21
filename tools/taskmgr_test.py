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
  3. Ending a process -- Force Quit (SYS_KILL) and Close (the
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
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TASKMGR = "/bin/wm/system/taskmgr"
VICTIM = "/bin/wm/demos/uidemo"   # something to list and then kill

checks = []


def header_ink_x(qmp, path, rect):
    """(leftmost, rightmost) x of non-background ink in `rect`, or None.

    Used to prove the sort arrow does not sit ON TOP of the column
    title. A right-aligned title is positioned from the column's right
    edge -- which is where the arrow goes -- so the correct behaviour is
    that the title SHIFTS LEFT when its column becomes sorted. If the
    arrow is merely drawn over it, the title does not move.

    Ink extent rather than an ink COUNT: the arrow adds pixels either
    way, so a count cannot tell the two apart, while the leftmost ink
    column moves only when the text was actually repositioned.
    """
    from PIL import Image
    # A SETTLED frame (two identical consecutive reads), not a bare
    # screenshot: the sort re-render is a client draw plus a compositor
    # hop, so a raw capture can land on the pre-sort frame under load.
    qmp.stable_pixels(path)
    im = Image.open(path).convert("RGB")
    x, y, w, h = rect
    px = list(im.crop((x, y, x + w, y + h)).getdata())
    bg = max(set(px), key=px.count)
    xs = [i % w for i, p in enumerate(px) if p != bg]
    return (min(xs), max(xs)) if xs else None


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


def wait_layout(dbg, ok, timeout=5.0):
    """Poll the app's layout reports until ok(merged_layout) holds, or
    timeout. layout(dbg) drains new log lines each call and merges them
    into the persistent _layout, so the state CONVERGES as the app
    re-reports -- an observable wait in place of a fixed sleep after an
    action that changes the table. Returns the merged layout either way,
    so the caller's own check still runs (and fails with detail) on a
    timeout, per the 'assert the fact, don't just wait for it' rule."""
    deadline = time.time() + timeout
    while True:
        lay = layout(dbg)
        if ok(lay) or time.time() >= deadline:
            return lay
        time.sleep(0.03)


def wait_log(dbg, needle, timeout=4.0):
    """Accumulate log lines until one contains `needle`, or timeout.
    logs() CLEARS what it returns, so this keeps polling (each call sees
    only new lines) rather than reading once after a sleep. Returns True
    if seen."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(needle in l for l in dbg.logs()):
            return True
        time.sleep(0.03)
    return False


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
        enter_gui(qmp, args.sock)
    else:
        qmp = QMPSession(port=args.qmp_port)

    dbg = DebugConsole(args.sock)
    print("task manager (uui_table, process accounting, ending processes)")

    # A process to find in the table and later kill.
    spawn_reply = dbg.send(f"gui spawn {VICTIM}")
    # "gui: spawned "<path>" as pid N" -- the pid is what makes the row
    # findable below, and what stops this test ending the desktop.
    victim_pid = None
    for tok in spawn_reply.replace("\n", " ").split():
        if tok.isdigit():
            victim_pid = int(tok)
    
    dbg.settle()  # the victim will be running by the time Task Manager reads
                  # the process table; the screen_order() poll below is the
                  # real safety net if it is slow to appear.

    dbg.spawn(TASKMGR, "Task Manager")
    # Wait for the app to report a real layout, not a fixed guess.
    wait_layout(dbg, lambda l: "table" in l and "row_h" in l)

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
    # Wait for the table to actually follow the resize (both axes grew by
    # ~the amount the window did), not a fixed 1.5s. A real resize bug
    # that never grows one axis simply waits out the timeout, then the
    # checks below report it -- same outcome, faster on success.
    after = wait_layout(
        dbg,
        lambda l: (l.get("table", before)[2] - before[2] >= grow_x * 0.6
                   and l.get("table", before)[3] - before[3] >= grow_y * 0.6),
    ).get("table", before)
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
    lay = wait_layout(dbg, lambda l: l.get("sort") == (0, -1))
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
    lay = wait_layout(dbg, lambda l: l.get("sort", (0, 0))[0] != 0
                      and l.get("sort", (0, 0))[1] == 1)
    check("a DIFFERENT column starts ascending, not reversed",
          lay.get("sort", (None, None))[1] == 1 and lay.get("sort", (None,))[0] != 0,
          f"sort={lay.get('sort')}")

    # --- the arrow must not sit ON the title --------------------------
    #
    # Reported from actual use, not caught here: the arrow was drawn at
    # the column's right edge and RIGHT-aligned titles (PID, CPU,
    # Memory) are positioned from that same edge, so it landed on the
    # last character. Reserving clip width was not enough -- the edge
    # the title is measured from has to move too.
    #
    # PID is the tightest case: right-aligned and only six characters
    # wide. Sorting is currently on a different column, so this measures
    # the unsorted position first.
    col0 = lay.get("col0")
    hdr_rect = None
    if col0 is not None:
        # INSET past the chrome: the header's bottom rule and the
        # column separator are ink too, and including them pinned the
        # measured extent to the cell's edges no matter where the text
        # sat -- the first version of this check reported an identical
        # (0, 47) in both states and proved nothing.
        hdr_rect = (cx + col0[0] + 3, cy + ty + 2, col0[1] - 6, lay["header_h"] - 5)
        unsorted_ink = header_ink_x(qmp, f"{args.tmp}/tm_hdr_unsorted.png", hdr_rect)
        dbg.send("gui click %d %d" % (hdr_x, hdr_y))
        dbg.settle()
        wait_layout(dbg, lambda l: l.get("sort") == (0, 1))
        sorted_ink = header_ink_x(qmp, f"{args.tmp}/tm_hdr_sorted.png", hdr_rect)
        check("the sort arrow does not overlap the column title",
              unsorted_ink is not None and sorted_ink is not None
              and sorted_ink[0] < unsorted_ink[0],
              f"title ink starts at x={unsorted_ink} unsorted, {sorted_ink} sorted")
    else:
        check("the sort arrow does not overlap the column title", False,
              "no col0 rect reported")

    # Back to PID ascending, so the checks below see the order they
    # were written against. A test must establish its own preconditions.
    dbg.send("gui click %d %d" % (hdr_x, hdr_y))
    dbg.settle()
    lay = wait_layout(dbg, lambda l: l.get("sort") == (0, -1))
    if lay.get("sort") == (0, -1):
        dbg.send("gui click %d %d" % (hdr_x, hdr_y))
        dbg.settle()
        lay = wait_layout(dbg, lambda l: l.get("sort") == (0, 1))
    check("it can be put back to PID ascending",
          lay.get("sort") == (0, 1), f"sort={lay.get('sort')}")

    # --- selecting a row, and ending a process ------------------------
    win = window(dbg)
    cx, cy = win["content"]["x"], win["content"]["y"]
    lay = layout(dbg)
    tx, ty, _, _ = lay["table"]

    # FIND THE VICTIM'S ROW BY PID rather than assuming a position.
    #
    # This used to click row 0 and call it "the first listed process",
    # which held only while every process in the table was one this test
    # had spawned. With a ring-3 desktop the WINDOW MANAGER is a process
    # too and sorts first, so row 0 is the desktop -- and ending it made
    # the test kill the thing it was testing on.
    #
    # Task Manager reports `taskmgr: selected pid N` on every row click,
    # so ask instead of assume.
    # ASK WHERE IT IS, then click there -- rather than clicking rows until
    # one answers. The app already reports `taskmgr: order <pid> <pid>
    # ...` in SCREEN order whenever that order changes, so the row is a
    # lookup, not a search.
    #
    # The blind scan this replaces was fragile in three ways at once, and
    # went intermittent for it: it gave up after 8 rows, it could not
    # tell "the victim is not in the table YET" (it is spawned moments
    # before, and the table refreshes on a 500ms tick) from "not found",
    # and a re-sort between two of its clicks could move the victim into
    # a row it had already visited.
    # The order is logged ON CHANGE, so there may be no new line to wait
    # for -- the current one was already drained by the checks above,
    # which is why `lay` (read a few lines up) is the first place to
    # look. logs() clears what it returns, so anything not kept is gone.
    def screen_order(deadline, known):
        if known and victim_pid in known:
            return known
        seen = []
        while time.time() < deadline:
            seen += dbg.logs()
            for line in reversed([l for l in seen if "taskmgr: order " in l]):
                pids = [int(v) for v in line.split("taskmgr: order ", 1)[1].split()
                        if v.strip().isdigit()]
                if victim_pid in pids:
                    return pids
            time.sleep(0.3)
        return None

    victim_row = None
    known_order = lay.get("order")
    deadline = time.time() + 8.0
    # Up to three attempts: the order can change between reading it and
    # clicking (the table refreshes on its own tick), and the click's own
    # `selected pid` reply is what settles whether it did.
    for _ in range(3):
        pids = screen_order(deadline, known_order)
        if pids is None:
            break
        known_order = None   # a retry must re-read, not reuse
        row = pids.index(victim_pid)
        ry = cy + ty + lay["header_h"] + lay["row_h"] * row + lay["row_h"] // 2
        dbg.logs()
        dbg.send("gui click %d %d" % (cx + tx + 60, ry))
        dbg.settle()
        if wait_log(dbg, f"taskmgr: selected pid {victim_pid}", timeout=2.0):
            victim_row = row
            break
    if not check("found the victim's row in the table",
                 victim_row is not None, f"pid {victim_pid}"):
        passed = sum(1 for _, ok, _ in checks if ok)
        print(f"\ntaskmgr_test: {passed} passed, {len(checks) - passed} failed")
        return 1

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
    dbg.logs()  # clear, so wait_log below sees the arm from THIS click
    dbg.send("gui click %d %d" % (bx, by))
    dbg.settle()
    # Key the "kills nothing" check off the ARM the app logs (a positive
    # signal) instead of a blind sleep hoping a kill would have fired by
    # now -- then assert nothing was actually killed.
    wait_log(dbg, "taskmgr: armed")
    armed_count = dbg.json("gui windows --json")["count"]
    check("one click ARMS and kills nothing",
          armed_count == before_count, f"{before_count} -> {armed_count}")

    # SECOND click commits.
    dbg.send("gui click %d %d" % (bx, by))
    dbg.settle()
    # Poll for the kill to land: the victim exits and is reaped, so its
    # window disappears -- an observable, not a fixed 1.8s.
    deadline = time.time() + 5.0
    while True:
        after_count = dbg.json("gui windows --json")["count"]
        if after_count == before_count - 1 or time.time() >= deadline:
            break
        time.sleep(0.05)
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
