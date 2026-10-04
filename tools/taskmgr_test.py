#!/usr/bin/env python3
"""Drive the ring-3 Task Manager: its three pages, the table, and its verbs.

WHAT IS UNDER TEST
------------------
  1. `uui_table` as Task Manager uses it: reflow with the window, SORTING
     (the ORDER, not the sort state -- the widget can record a sort it
     never applies), and the three views over the same rows: Grouped
     (Apps / Background / System), Tree (parents), List. The order comes
     from the app's own `taskmgr: order` line, in SCREEN order -- reading
     it from pixels would mean OCR.
  2. The per-process accounting behind it: a spawned process appears,
     grouped as an app because it has a desktop entry, nested under the
     desktop that started it.
  3. The verbs: Stop/Continue (SIGSTOP/SIGCONT, checked through `ps`, an
     independent view) and Force Quit, which ARMS on the first click and
     commits on the second.
  4. The Performance page lists devices and shows the Ethernet card's
     connection log; the Services page stops and starts a service,
     checked through `service list`.

THE BUG THIS FILE EXISTS FOR FIRST: the table once grew in WIDTH with its
window and by 16 px of height against 300 (a button group measured its
natural height from the origin). So the resize check asserts the table
grew by ROUGHLY WHAT THE WINDOW GREW BY, not merely that it changed.

AND A TRAP ALREADY PAID FOR: a button reports press, motion and release
to its app, and only the release is the click. The first build of the
rail layout sent SIGSTOP four times per click and four init requests per
service click, three of which timed out. The Services check below asserts
ONE `service stop` line per click for that reason.

Geometry comes from the app's own `taskmgr: layout ...` lines; nothing
here derives a row offset except from the reported row height.

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
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

TASKMGR = "/bin/wm/system/taskmgr"
VICTIM = "/bin/wm/demos/uidemo"   # has a desktop entry, so it is an APP
SERVICE = "ntpd"                  # idle unless system.ntp is on: safe to stop

K_DOWN = "0x92"
K_UP = "0x91"
K_BACKSPACE = "0x08"



_res = Results()
check = _res.check
checks = _res.rows


def header_ink_x(qmp, path, rect):
    """(leftmost, rightmost) x of non-background ink in `rect`, or None.

    Proves the sort arrow does not sit ON the column title: a
    right-aligned title must SHIFT LEFT when its column becomes sorted.
    Ink extent rather than a count -- the arrow adds pixels either way."""
    from PIL import Image
    qmp.stable_pixels(path)
    im = Image.open(path).convert("RGB")
    x, y, w, h = rect
    px = list(im.crop((x, y, x + w, y + h)).getdata())
    bg = max(set(px), key=px.count)
    xs = [i % w for i, p in enumerate(px) if p != bg]
    return (min(xs), max(xs)) if xs else None


# Merged across calls: DebugConsole.logs() CLEARS what it returns, so a
# key reported once would vanish from a second read. Last value wins.
_layout = {}
_lines = []


def layout(dbg):
    out = _layout
    for line in dbg.logs():
        _lines.append(line)
        m = re.search(r"taskmgr: layout (\w+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"taskmgr: layout (\w+)\.(row_h|header_h|item_h|count|shown) (-?\d+)", line)
        if m:
            out[f"{m.group(1)}.{m.group(2)}"] = int(m.group(3))
        m = re.search(r"taskmgr: layout (\w+)\.(col|slot) (\d+) (-?\d+) (-?\d+) (\d+) (\d+)", line)
        if m:
            out[f"{m.group(1)}.{m.group(2)}{m.group(3)}"] = tuple(int(v) for v in m.groups()[3:])
        m = re.search(r"taskmgr: layout memcomp\.seg (\d+) (-?\d+) (-?\d+) (\d+) (\d+)", line)
        if m:
            out[f"memcomp.seg{m.group(1)}"] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"taskmgr: rows (\d+)", line)
        if m:
            out["rows"] = int(m.group(1))
        m = re.search(r"taskmgr: sort col (-?\d+) dir (-?\d+) view (\w+)", line)
        if m:
            out["sort"] = (int(m.group(1)), int(m.group(2)))
            out["viewmode"] = m.group(3)
        m = re.search(r"taskmgr: order (.*)$", line)
        if m:
            # NEGATIVE ids too: the kind rows (Kernel, Graphics, Shared
            # memory) are rows on screen, and dropping them would shift
            # every row index below them.
            out["order"] = [int(v) for v in m.group(1).split() if re.fullmatch(r"-?\d+", v)]
        m = re.search(r"taskmgr: page (\d+)", line)
        if m:
            out["page"] = int(m.group(1))
        m = re.search(r"taskmgr: services (\d+)", line)
        if m:
            out["svc_count"] = int(m.group(1))
        m = re.search(r"taskmgr: device (\d+) (\w+)", line)
        if m:
            out["device"] = (int(m.group(1)), m.group(2))
    return out


def wait_layout(dbg, ok, timeout=5.0):
    deadline = time.time() + timeout
    while True:
        lay = layout(dbg)
        if ok(lay) or time.time() >= deadline:
            return lay
        time.sleep(0.05)


def wait_log(dbg, needle, timeout=4.0):
    """True once a line containing `needle` has been seen since the last
    call to mark(). Keeps every line in _lines, so layout() loses nothing."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        layout(dbg)
        if any(needle in l for l in _lines):
            return True
        time.sleep(0.05)
    return False


def meminfo_kinds(dbg):
    """meminfo's "used" and its four kinds, in bytes, and the raw text."""
    mi = dbg.send("sh meminfo") or ""
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}

    def size(t):
        return float(t[:-1]) * mult[t[-1]] if t[-1] in mult else float(t)
    m = re.search(r"used:\s+\d+\s+([\d.]+[KMG]?)", mi)
    if not m:
        return None, None, mi
    kinds = {k: size(v) for k, v in re.findall(r"^\s+(apps|shared|graphics|kernel):\s+(\S+)",
                                                mi, re.M)}
    return size(m.group(1)), kinds, mi


def mark():
    _lines.clear()


def window(dbg, title="Task Manager"):
    wins = [w for w in dbg.json("gui windows --json")["windows"] if w["title"] == title]
    return wins[-1] if wins else None


def proc_state(dbg, pid):
    for line in (dbg.send("sh ps") or "").splitlines():
        f = line.split()
        if f and f[0] == str(pid):
            return f[3]
    return None


def service_state(dbg, name):
    for line in (dbg.send("sh service list") or "").splitlines():
        f = line.split()
        if f and f[0] == name:
            return f[1]
    return None


def finish():
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print(f"\ntaskmgr_test: {passed} passed, {failed} failed")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "taskmgr_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("task manager (pages, uui_table groups/tree, the verbs)")

    victim_pid = None
    for tok in (dbg.send(f"gui spawn {VICTIM}") or "").replace("\n", " ").split():
        if tok.isdigit():
            victim_pid = int(tok)
    dbg.settle()
    desktop_pid = next((int(line.split()[0]) for line in (dbg.send("sh ps") or "").splitlines()
                        if line.split()[-1:] == ["toywm"]), None)

    dbg.spawn(TASKMGR, "Task Manager")
    lay = wait_layout(dbg, lambda l: "table" in l and "table.row_h" in l and "order" in l)

    win = window(dbg)
    if not check("Task Manager opened, on its Processes page",
                 win is not None and lay.get("page") == 0, f"page={lay.get('page')}"):
        return finish()
    check("it is a ring-3 client", win["client_pid"] > 0, f"pid {win['client_pid']}")
    if not check("it reported its layout", "table" in lay and "view" in lay and "viewmode" in lay):
        return finish()
    cx, cy = win["content"]["x"], win["content"]["y"]

    def click(rect_or_xy):
        x, y = rect_or_xy
        dbg.send("gui click %d %d" % (cx + x, cy + y))
        dbg.settle()

    def centre(r):
        return (r[0] + r[2] // 2, r[1] + r[3] // 2)

    def row_y(view_index):
        tx, ty, _, _ = _layout["table"]
        return ty + _layout["table.header_h"] + _layout["table.row_h"] * view_index \
            + _layout["table.row_h"] // 2

    check("it lists more than one process", lay.get("rows", 0) >= 2, f"{lay.get('rows')} rows")

    # --- the Grouped view (the default) --------------------------------
    order = wait_layout(dbg, lambda l: victim_pid in l.get("order", [])).get("order", [])
    check("the spawned process is listed", victim_pid in order, f"order={order}")
    check("Grouped: an app sorts ABOVE init (Apps first, System last)",
          victim_pid in order and 1 in order and order.index(victim_pid) < order.index(1),
          f"order={order}")

    # --- List view, and sorting ------------------------------------------
    def set_view(i, name):
        click(centre(_layout[f"view.slot{i}"]))
        return wait_layout(dbg, lambda l: l.get("viewmode") == name)

    lay = set_view(2, "List")
    check("the List view is selected", lay.get("viewmode") == "List", f"view={lay.get('viewmode')}")
    hdr_y = _layout["table"][1] + _layout["table.header_h"] // 2
    pid_col = _layout.get("table.col1")
    name_col = _layout.get("table.col0")
    if not check("it reports each column's rect", pid_col and name_col):
        return finish()

    check("it starts sorted by Name ascending", lay.get("sort") == (0, 1), f"sort={lay.get('sort')}")
    click((pid_col[0] + pid_col[2] // 2, hdr_y))
    lay = wait_layout(dbg, lambda l: l.get("sort") == (1, 1))
    asc = lay.get("order", [])
    # The kind rows (negative ids) trail in EITHER direction; the
    # processes before them are what the sort orders.
    procs = [p for p in asc if p > 0]
    tail = asc[len(procs):]
    check("clicking PID sorts by it, ascending", lay.get("sort") == (1, 1) and procs == sorted(procs)
          and asc[:len(procs)] == procs, f"sort={lay.get('sort')} order={asc}")
    click((pid_col[0] + pid_col[2] // 2, hdr_y))
    lay = wait_layout(dbg, lambda l: l.get("sort") == (1, -1))
    desc = lay.get("order", [])
    check("clicking it again REVERSES it", lay.get("sort") == (1, -1)
          and desc == sorted(procs, reverse=True) + tail,
          f"sort={lay.get('sort')} order={desc}")

    # The arrow must not sit ON the right-aligned PID title: measured
    # unsorted (sort moved to Name), then sorted.
    click((name_col[0] + name_col[2] // 2, hdr_y))
    lay = wait_layout(dbg, lambda l: l.get("sort") == (0, 1))
    check("a DIFFERENT column starts ascending", lay.get("sort") == (0, 1), f"sort={lay.get('sort')}")
    ty = _layout["table"][1]
    rect = (cx + pid_col[0] + 3, cy + ty + 2, pid_col[2] - 6, _layout["table.header_h"] - 5)
    unsorted_ink = header_ink_x(qmp, f"{args.tmp}/tm_hdr_unsorted.png", rect)
    click((pid_col[0] + pid_col[2] // 2, hdr_y))
    lay = wait_layout(dbg, lambda l: l.get("sort") == (1, 1))
    sorted_ink = header_ink_x(qmp, f"{args.tmp}/tm_hdr_sorted.png", rect)
    check("the sort arrow does not overlap the column title",
          unsorted_ink and sorted_ink and sorted_ink[0] < unsorted_ink[0],
          f"title ink {unsorted_ink} unsorted, {sorted_ink} sorted")

    # --- the Tree view, and folding ----------------------------------------
    lay = set_view(1, "Tree")
    order = wait_layout(dbg, lambda l: l.get("viewmode") == "Tree" and "order" in l).get("order", [])
    check("Tree: init is the first row", order[:1] == [1], f"order={order}")
    if desktop_pid and desktop_pid in order and victim_pid in order:
        check("Tree: the spawned app is under the desktop that started it",
              order.index(desktop_pid) < order.index(victim_pid), f"order={order}")
        # Fold the desktop: its row's expander is one indent in (init is
        # depth 0, the desktop depth 1); an indent is the font's height.
        indent = _layout["table.row_h"] - 4
        d = order.index(desktop_pid)
        mark()
        click((_layout["table.col0"][0] + 4 + indent + indent // 2, row_y(d)))
        folded = wait_layout(dbg, lambda l: victim_pid not in l.get("order", [victim_pid]))
        check("clicking the desktop's expander folds its children away",
              wait_log(dbg, f"toggled pid {desktop_pid}", 1.0)
              and victim_pid not in folded.get("order", []),
              f"order={folded.get('order')}")
        # AN ARM MUST NOT FOLLOW A MOVED SELECTION. With the desktop
        # folded, arm Force Quit on its child in List view, then go back
        # to Tree: the table moves the selection to the desktop, and the
        # arm must be dropped -- or the next "Confirm?" kills the desktop.
        # Asserted on the app's disarm line, never by clicking Confirm.
        set_view(2, "List")
        lo = wait_layout(dbg, lambda l: victim_pid in l.get("order", []))
        mark()
        click((_layout["table"][0] + 60, row_y(lo["order"].index(victim_pid))))
        wait_log(dbg, f"selected pid {victim_pid}", 2.0)
        click(centre(_layout["btn_kill"]))
        armed = wait_log(dbg, f"armed kill pid {victim_pid}", 2.0)
        set_view(1, "Tree")
        check("an armed Force Quit is DROPPED when folding moves the selection",
              armed and wait_log(dbg, f"disarmed, selection moved to pid {desktop_pid}", 2.0),
              f"armed={armed}")
        order = wait_layout(dbg, lambda l: l.get("viewmode") == "Tree").get("order", order)
        d = order.index(desktop_pid)
        click((_layout["table.col0"][0] + 4 + indent + indent // 2, row_y(d)))
        opened = wait_layout(dbg, lambda l: victim_pid in l.get("order", []))
        check("...and clicking it again brings them back", victim_pid in opened.get("order", []),
              f"order={opened.get('order')}")

    # --- the filter ----------------------------------------------------------
    lay = set_view(2, "List")
    click(centre(_layout["search"]))
    for ch in "uidemo":
        dbg.key(ch)
    lay = wait_layout(dbg, lambda l: l.get("order") == [victim_pid])
    check("typing in the filter leaves only the matching process",
          lay.get("order") == [victim_pid], f"order={lay.get('order')}")
    for _ in range(6):
        dbg.key(K_BACKSPACE)
    lay = wait_layout(dbg, lambda l: len(l.get("order", [])) >= 2)
    check("...and clearing it brings every process back", len(lay.get("order", [])) >= 2,
          f"order={lay.get('order')}")

    # --- resize: the table follows in BOTH axes -----------------------------
    win = window(dbg)
    before = _layout["table"]
    grip = (win["x"] + win["w"] - 1 + 4, win["y"] + win["h"] - 1 + 4)
    # As far as the screen allows: a window that opens wide has less room.
    scr = dbg.state()["screen"]
    grow_x = max(40, min(120, scr["w"] - (win["x"] + win["w"]) - 8))
    grow_y = max(40, min(160, scr["h"] - (win["y"] + win["h"]) - 60))
    dbg.send("gui drag %d %d %d %d" % (grip[0], grip[1], grip[0] + grow_x, grip[1] + grow_y))
    dbg.settle()
    after = wait_layout(dbg, lambda l: l["table"][2] - before[2] >= grow_x * 0.6
                        and l["table"][3] - before[3] >= grow_y * 0.6)["table"]
    check("the table WIDENS with the window", after[2] - before[2] >= grow_x * 0.6,
          f"+{after[2] - before[2]}px of ~{grow_x}")
    check("the table GROWS TALLER with the window", after[3] - before[3] >= grow_y * 0.6,
          f"+{after[3] - before[3]}px of ~{grow_y}")
    win = window(dbg)
    cx, cy = win["content"]["x"], win["content"]["y"]

    # --- selecting the victim: by its place in the reported order -----------
    lay = wait_layout(dbg, lambda l: victim_pid in l.get("order", []))
    order = lay.get("order", [])
    mark()
    if victim_pid in order:
        click((_layout["table"][0] + 60, row_y(order.index(victim_pid))))
    if not check("clicking its row selects it",
                 wait_log(dbg, f"selected pid {victim_pid}", 2.0), f"order={order}"):
        return finish()

    # The keyboard reaches the table: an arrow moves off the row, and a
    # letter seeks back by NAME (the seek column is Name, not PID).
    at_end = order.index(victim_pid) == len(order) - 1
    mark()
    dbg.key(K_UP if at_end else K_DOWN)
    check("an arrow key reaches the table", wait_log(dbg, "selected pid ", 2.0))
    mark()
    dbg.key("u")
    check("typing a letter seeks by NAME",
          wait_log(dbg, f"selected pid {victim_pid}", 2.0), "`u` did not select uidemo")

    # --- Stop and Continue ---------------------------------------------------
    stop = _layout.get("btn_stop")
    if check("it reported its buttons", stop and "btn_kill" in _layout):
        mark()
        click(centre(stop))
        wait_log(dbg, f"stopped pid {victim_pid}", 2.0)
        time.sleep(0.5)
        layout(dbg)
        check("Stop stops it (ps says so)", proc_state(dbg, victim_pid) == "stopped",
              f"ps state {proc_state(dbg, victim_pid)}")
        check("...with ONE signal per click",
              sum(1 for l in _lines if f"stopped pid {victim_pid}" in l) == 1,
              f"{sum(1 for l in _lines if f'stopped pid {victim_pid}' in l)} lines")
        click(centre(stop))
        wait_log(dbg, f"continued pid {victim_pid}", 2.0)
        check("the same button now continues it", proc_state(dbg, victim_pid) not in (None, "stopped"),
              f"ps state {proc_state(dbg, victim_pid)}")

        # --- Force Quit: arm, then commit ---------------------------------
        before_count = dbg.json("gui windows --json")["count"]
        mark()
        click(centre(_layout["btn_kill"]))
        wait_log(dbg, "taskmgr: armed")
        check("one click ARMS and kills nothing",
              dbg.json("gui windows --json")["count"] == before_count)
        click(centre(_layout["btn_kill"]))
        deadline = time.time() + 5.0
        while dbg.json("gui windows --json")["count"] != before_count - 1 and time.time() < deadline:
            time.sleep(0.05)
        check("the second click ends the process",
              dbg.json("gui windows --json")["count"] == before_count - 1)
        check("...and says so in the log", wait_log(dbg, f"killed pid {victim_pid}", 1.0))

    # --- the context menu's "Go to service" -----------------------------
    #
    # It once only switched pages, leaving the user to find the service.
    # netd is a service; its row is found by pid in the reported order,
    # and the menu's LAST row is chosen by keyboard (Up from none wraps
    # to it), so no menu geometry is derived here.
    netd = next((int(l.split()[0]) for l in (dbg.send("sh ps") or "").splitlines()
                 if l.split()[-1:] == ["netd"]), None)
    # An order WITHOUT the process just force-quit: the one before it
    # lists the victim, and every row after it is one off.
    lay = wait_layout(dbg, lambda l: netd in l.get("order", [])
                      and victim_pid not in l.get("order", []))
    if netd in lay.get("order", []) and lay.get("viewmode") == "List":
        mark()
        y = row_y(lay["order"].index(netd))
        dbg.send("gui rclick %d %d" % (cx + _layout["table"][0] + 60, cy + y))
        dbg.settle()
        # The menu is the APP's popup, opened on the release: a client
        # round trip after the WM goes quiet, so settle() alone is early
        # and the key lands on the table instead.
        time.sleep(0.8)
        dbg.key(K_UP)
        dbg.key("0x0a")
        lay = wait_layout(dbg, lambda l: l.get("page") == 2, timeout=3.0)
        check("'Go to service' opens Services WITH that service selected",
              lay.get("page") == 2 and wait_log(dbg, "service selected netd", 2.0),
              f"page={lay.get('page')} netd={netd} row={lay['order'].index(netd)} "
              f"log: {[l.split('] ')[-1] for l in _lines if 'taskmgr:' in l and 'layout' not in l][-6:]}")
        rail = _layout.get("rail")
        rh = _layout.get("rail.row_h")
        if rail and rh:   # back to Processes, where the next section starts
            click((rail[0] + rail[2] // 2, rail[1] + rh // 2))
            wait_layout(dbg, lambda l: l.get("page") == 0)
    else:
        check("netd is listed, to go to its service", False, f"netd={netd}")

    # --- memory that adds up -------------------------------------------------
    # The text oracle first: meminfo's four kinds must sum to its "used".
    used, kinds, mi = meminfo_kinds(dbg)
    check("meminfo lists four kinds of memory in use", kinds and len(kinds) == 4, mi[-300:])
    if used and kinds and len(kinds) == 4:
        # Kernel is the remainder, so "they sum to used" would hold by
        # construction. These can fail: each names memory a broken count
        # would misplace.
        scr = dbg.state()["screen"]
        driver = re.search(r"Driver:\s+(\S+)", dbg.send("sh lsdisplay") or "")
        if driver and driver.group(1) == "bochs":
            # bochs holds no RAM of its own: Graphics is the console's
            # back buffer, to the page.
            back = (scr["w"] * scr["h"] * 4 + 4095) // 4096 * 4096
            check("Graphics is exactly the console's back buffer (bochs)",
                  abs(kinds["graphics"] - back) < 64 << 10,
                  f"graphics {kinds['graphics']:.0f} back buffer {back}")
        tm = window(dbg)
        if tm:
            bufs = 2 * tm["content"]["w"] * tm["content"]["h"] * 4
            check("Shared holds at least Task Manager's own two window buffers",
                  kinds["shared"] >= bufs * 0.98, f"shared {kinds['shared']:.0f} buffers {bufs}")
        check("...with the apps and the kernel both non-zero",
              kinds["apps"] > 0 and kinds["kernel"] > 0, str(kinds))

    # The kind rows: listed, selectable, and beyond every verb.
    lay = set_view(2, "List")
    order = wait_layout(dbg, lambda l: -1 in l.get("order", [])).get("order", [])
    check("Processes lists Kernel, Graphics and Shared memory as rows",
          all(k in order for k in (-1, -2, -3)), f"order={order}")
    if -1 in order and "btn_kill" in _layout:
        mark()
        click((_layout["table"][0] + 60, row_y(order.index(-1))))
        check("the Kernel row can be selected", wait_log(dbg, "selected pid -1", 2.0))
        mark()
        click(centre(_layout["btn_kill"]))
        click(centre(_layout["btn_kill"]))
        time.sleep(0.5)
        armed = wait_log(dbg, "armed kill pid -1", 0.5) or wait_log(dbg, "killed pid -1", 0.5)
        check("Force Quit does nothing to a kind row", not armed)

    # --- Performance -------------------------------------------------------
    rail = _layout.get("rail")
    rh = _layout.get("rail.row_h")
    if check("it reported its navigation rail", rail and rh):
        click((rail[0] + rail[2] // 2, rail[1] + rh + rh // 2))
        lay = wait_layout(dbg, lambda l: l.get("page") == 1 and "devices" in l)
        check("the rail opens Performance", lay.get("page") == 1, f"page={lay.get('page')}")
        n = lay.get("devices.count", 0)
        check("it lists CPU, Memory, Disk and at least one network card", n >= 4, f"{n} devices")
        if n >= 4:
            dv, ih = _layout["devices"], _layout["devices.item_h"]
            # Memory: the composition bar, held against meminfo's numbers.
            click((dv[0] + dv[2] // 2, dv[1] + ih + ih // 2))
            lay = wait_layout(dbg, lambda l: l.get("device", (0,))[0] == 1 and "memcomp.seg3" in l)
            segs = [lay.get(f"memcomp.seg{i}") for i in range(4)]
            if check("Memory shows a four-part composition bar", all(segs) and "memcomp" in lay,
                     f"segs={segs}"):
                bar = lay["memcomp"]
                widths = [sg[2] for sg in segs]
                check("...whose parts fill the bar edge to edge",
                      segs[0][0] == bar[0] + 1 and segs[3][0] + segs[3][2] == bar[0] + bar[2] - 1,
                      f"bar={bar} segs={segs}")
                # Read again NOW: the numbers drift while the test runs.
                _, kinds, _ = meminfo_kinds(dbg)
                if kinds and len(kinds) == 4:
                    vals = [kinds[k] for k in ("apps", "shared", "graphics", "kernel")]
                    inner = bar[2] - 2
                    want = [inner * v / sum(vals) for v in vals]
                    check("...in meminfo's proportions (each part within 4 px)",
                          all(abs(w - x) <= 4 for w, x in zip(widths, want)),
                          f"widths={widths} want={[round(x) for x in want]}")
            click((dv[0] + dv[2] // 2, dv[1] + 3 * ih + ih // 2))
            lay = wait_layout(dbg, lambda l: l.get("device", (0,))[0] == 3 and "connlog" in l)
            check("selecting the card shows Ethernet and its connection log",
                  lay.get("device") == (3, "Ethernet") and "connlog" in lay,
                  f"device={lay.get('device')}")

        # --- Services -----------------------------------------------------
        click((rail[0] + rail[2] // 2, rail[1] + 2 * rh + rh // 2))
        lay = wait_layout(dbg, lambda l: l.get("page") == 2 and "svctable" in l
                          and "svctable.row_h" in l)
        check("the rail opens Services, listing init's services",
              lay.get("page") == 2 and lay.get("svc_count", 0) >= 5, f"{lay.get('svc_count')} services")
        names = sorted(line.split()[0] for line in (dbg.send("sh service list") or "").splitlines()
                       if line and not line.startswith("#") and len(line.split()) > 2)
        if SERVICE in names and service_state(dbg, SERVICE) == "running":
            st = _layout["svctable"]
            y = st[1] + _layout["svctable.header_h"] + _layout["svctable.row_h"] * names.index(SERVICE) \
                + _layout["svctable.row_h"] // 2
            mark()
            click((st[0] + 40, y))
            check(f"clicking {SERVICE}'s row selects it", wait_log(dbg, f"service selected {SERVICE}", 2.0))
            mark()
            click(centre(_layout["svc_stop"]))
            deadline = time.time() + 5.0
            while service_state(dbg, SERVICE) != "stopped" and time.time() < deadline:
                time.sleep(0.2)
            check("Stop stops the service (`service list` says so)",
                  service_state(dbg, SERVICE) == "stopped", service_state(dbg, SERVICE))
            wait_log(dbg, f"service stop {SERVICE}", 2.0)
            time.sleep(0.5)   # a duplicate would follow within the same click
            layout(dbg)
            check("...with ONE request per click",
                  sum(1 for l in _lines if f"service stop {SERVICE}" in l) == 1,
                  "; ".join(l for l in _lines if "service stop" in l))
            # Task Manager's own view must have caught up, or Start is
            # still greyed out and the click is rightly ignored.
            wait_log(dbg, f"service {SERVICE} state stopped", 3.0)
            click(centre(_layout["svc_start"]))
            deadline = time.time() + 5.0
            while service_state(dbg, SERVICE) != "running" and time.time() < deadline:
                time.sleep(0.2)
            ok = service_state(dbg, SERVICE) == "running"
            why = "" if ok else " | " + " ; ".join(
                l.strip() for l in (dbg.send("sh dmesg") or "").splitlines()
                if "taskmgr: service" in l or "taskmgr: layout svc_start" in l)[-600:]
            check("Start brings it back", ok, f"{service_state(dbg, SERVICE)}{why} | "
                  f"clicked svc_start at {_layout.get('svc_start')}")
        else:
            check(f"{SERVICE} is listed and running, to stop and start", False, f"{names}")

    # --- NARROW: the Name column keeps its room ----------------------------
    #
    # The details pane steps aside when the table would squeeze Name. A
    # DRAG cannot test it: the window will not shrink below its natural
    # width. A REMEMBERED size can go below it -- the laptop reopened
    # Task Manager at a size saved before this layout, under a larger
    # face, and Name came out zero pixels wide -- so the size is seeded
    # the same way (/etc/windows.conf, keyed by app_id) and the app is
    # opened again into it.
    tm = window(dbg)
    if tm:
        dbg.send(f"sh kill {tm['client_pid']}")
        deadline = time.time() + 5.0
        while window(dbg) and time.time() < deadline:
            time.sleep(0.1)
    # WRITTEN, not appended, and after a pause: the desktop records the
    # closing window's own geometry, and a line appended before that
    # write loses to it.
    time.sleep(1.0)
    dbg.send("sh write /etc/windows.conf taskmgr=40,60,600,420")
    time.sleep(0.5)
    _layout.clear()
    dbg.spawn(TASKMGR, "Task Manager")
    narrow = wait_layout(dbg, lambda l: "table.col0" in l and "table.row_h" in l
                         and "btn_kill" in l
                         and l["table.col0"][2] >= 4 * l["table.row_h"], timeout=5.0)
    w = window(dbg)
    name_w = narrow.get("table.col0", (0, 0, 0, 0))[2]
    bk = narrow.get("btn_kill", (0, 0, 0, 0))
    check("a remembered 600 px size: Name keeps its room",
          w is not None and name_w >= 4 * narrow.get("table.row_h", 99),
          f"window {w and w['w']} px, Name {name_w} px")
    # ...and the toolbar is all there: the window's minimum clamps a size
    # that would push Force Quit past the edge.
    check("...and Force Quit is inside the window",
          w is not None and bk[0] + bk[2] <= w["content"]["w"],
          f"Force Quit ends at {bk[0] + bk[2]} of {w and w['content']['w']}")

    # --- it survived its own operation -----------------------------------
    check("Task Manager is still running", window(dbg) is not None)
    check("the desktop is still alive", dbg.json("gui state --json") is not None)
    return finish()


if __name__ == "__main__":
    sys.exit(main())
