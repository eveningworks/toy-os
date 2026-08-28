#!/usr/bin/env python3
"""Drive the File Manager (userland/gui/apps/files.c) and assert on it.

WHAT THIS IS ACTUALLY FOR
-------------------------
Three things, and only the first is about the app:

1. **Two panes, and operations between them.** Copy, move, delete,
   mkdir and rename, driven by the keys a commander uses.

2. **THE RESULT IS CHECKED THROUGH `ls`, NOT THROUGH THE APP.** Every
   file operation here is asserted by asking the SHELL what is on disk
   afterwards. Reading the app's own pane back would pass on a manager
   that updated its list and never spawned anything -- the app is the
   thing under test, so it cannot also be the witness.

3. **`uui_fileview` works at all.** The widget is shared with Image
   Viewer, Notepad's dialog and the WM's file picker, so the descend /
   go-up / ordering checks here are coverage for all four.

THE CHECKS THAT WOULD SURVIVE A BROKEN VERSION, AND WHY THEY DON'T
------------------------------------------------------------------
docs/gui-guidelines.md's rule is to ask what a broken version would
still pass.

  * "F8 deleted the file" passes on a manager with NO confirmation at
    all. So the delete check is a PAIR: F8 then Esc must leave the file
    on disk, and only then does F8-then-Enter remove it. The first half
    is the one that fails on a missing modal.
  * "the copy arrived" passes on a single-file copy when two files were
    marked. So the marking check copies TWO and requires BOTH, with the
    third file in the directory required ABSENT -- otherwise "it copied
    everything" would pass too.
  * "Backspace went up" passes on an app that just jumps to the root. So
    it also requires the directory just LEFT to be the selected row on
    arrival, which only a correct up() produces.
  * "the active pane is marked" passes on a paint that marks both. So
    the pixel check samples the OTHER pane's header as its control and
    requires the two to DIFFER.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/filemanager_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TITLE = "File Manager"
SPAWN_PATH = "/bin/wm/apps/files"
SRC = "/fmtest"
DST = "/fmdest"
FILES_CONF = "/etc/files.conf"

# `gui key` takes a character or a code (userland/wm/wm_debug.c), which
# is why nothing here depends on the guest's keyboard layout -- the trap
# every QMP-typing tool here has paid for at least once.
K_TAB, K_ENTER, K_ESC, K_BACKSPACE = "0x09", "0x0a", "0x1b", "0x08"
K_DOWN, K_UP = "0x92", "0x91"
K_INSERT = "0xb3"
K_HOME = "0x97"
K_F2, K_F5, K_F6, K_F7, K_F8 = "0x9a", "0xac", "0xad", "0xae", "0xaf"
K_F10, K_LEFT, K_RIGHT = "0xa4", "0x95", "0x96"


class Layout:
    """Geometry and state as the CLIENT reports it -- never re-derived.

    files.c logs `files: layout <what> ...`, content-relative.
    """

    def __init__(self, content, lines):
        # ONLY THE LAST FRAME. A batch of log lines can hold several
        # draws, and reading each field's last occurrence across the
        # whole batch mixes them -- which is how this tool once reported
        # the newly-active pane's index beside the OLD pane's selection
        # and deleted the wrong assumption. `layout pane 0` is emitted
        # first on every draw, so it is the frame boundary (the same
        # trick imgview_test.py's parser documents).
        marks = [i for i, l in enumerate(lines) if "files: layout pane 0 " in l]
        if marks:
            lines = lines[marks[-1]:]
        self.ox, self.oy = content["x"], content["y"]
        self.pane = {}
        self.dir = {}
        self.rows = {}
        self.marked = (0, 0)
        self.view = None      # [mode0, mode1, single, tree_on, tree_nodes]
        self.treebox = None   # [x, y, w, h, row_h, selected_id]
        self.active = None
        self.selected = None
        self.modal = None
        for line in lines:
            if "files: layout " not in line:
                continue
            p = line.split("files: layout ", 1)[1].split()
            if p[0] == "pane" and len(p) >= 6:
                self.pane[int(p[1])] = tuple(int(v) for v in p[2:6])
            elif p[0] == "dir" and len(p) >= 3:
                self.dir[int(p[1])] = p[2]
            elif p[0] == "rows" and len(p) >= 3:
                self.rows[int(p[1])] = int(p[2])
            elif p[0] == "active" and len(p) >= 2:
                self.active = int(p[1])
            elif p[0] == "selected" and len(p) >= 2:
                self.selected = p[1]
            elif p[0] == "modal" and len(p) >= 2:
                self.modal = int(p[1])
            elif p[0] == "marked" and len(p) >= 3:
                self.marked = (int(p[1]), int(p[2]))
            elif p[0] == "view" and len(p) >= 8:
                # "view <mode0> <mode1> single <s> tree <t> <nodes>"
                self.view = [int(p[1]), int(p[2]), int(p[4]),
                             int(p[6]), int(p[7])]
            elif p[0] == "treebox" and len(p) >= 7:
                self.treebox = [int(v) for v in p[1:7]]

    def complete(self):
        return 0 in self.pane and 1 in self.pane and self.active is not None

    def pane_centre(self, i):
        x, y, w, h = self.pane[i]
        return (self.ox + x + w // 2, self.oy + y + h // 2)

    def header_point(self, i):
        """A pixel inside pane i's path strip, which is drawn ABOVE the
        pane's own rect (files.c's draw_pane_headers)."""
        x, y, w, _ = self.pane[i]
        return (self.ox + x + w // 2, self.oy + y - 3)


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def listing(dbg, path):
    """What the SHELL says is in `path` -- the independent witness."""
    out = dbg.send(f"sh ls {path}") or ""
    names = []
    for line in out.splitlines():
        line = line.strip()
        # The kernel narrates every ring-3 load on the same console
        # ("elf_run: ...", "syscall: ..."), and the console can echo the
        # command itself -- anything with a colon or a space is not a
        # filename here.
        if not line or ":" in line or " " in line or line.startswith("---"):
            continue
        names.append(line.rstrip("/"))
    return names


def layout_now(dbg, win, tries=25):
    """The app's CURRENT layout report, polled rather than slept for."""
    for _ in range(tries):
        # NOT cleared first: the app draws when something happens, so
        # the report we want may already be in the buffer -- an
        # unconditional clear here threw away the only frame the app had
        # logged and reported a working window as silent.
        lines = dbg.logs("files:", clear=True)
        if not lines:
            time.sleep(0.2)
            continue
        lay = Layout(win["content"], lines)
        if lay.complete():
            return lay
        time.sleep(0.2)
    return None


def wait_layout(dbg, win, pred, timeout=12.0):
    """Poll the app's own report until `pred` holds. Waiting on the
    OBSERVABLE rather than on a fixed sleep (CLAUDE.md)."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        lines = dbg.logs("files:", clear=True)
        if lines:
            lay = Layout(win["content"], lines)
            if lay.complete():
                last = lay
                if pred(lay):
                    return lay
        time.sleep(0.2)
    return last


def wait_listing(dbg, path, pred, timeout=25.0):
    """Poll `ls` until it satisfies `pred`. A spawned /bin/cp is a whole
    process: it has to be scheduled, read and write before the answer
    changes, and how long that takes varies with load."""
    deadline = time.time() + timeout
    names = []
    while time.time() < deadline:
        names = listing(dbg, path)
        if pred(names):
            return names
        time.sleep(0.5)
    return names


def menu_pick(dbg, *steps):
    """Drive the app's menu bar by keyboard: F10 opens the first menu
    with its first item hot, then each step. Ends with K_ENTER in the
    caller's list. Keyboard, not pixels: the menu's geometry is the
    toolkit's business."""
    dbg.key(K_F10)
    time.sleep(0.15)
    for k in steps:
        dbg.key(k)
        time.sleep(0.12)
    time.sleep(0.3)


def band_drag(dbg, qmp, x0, y0, x1, y1, steps=4):
    """A drag whose ENDPOINTS are confirmed, not assumed. qmp.drag()'s
    open-loop goto lands a large jump about a third of the way
    (warp_cursor's own measurement), so a rubber band swept with it
    covers some unknown smaller rectangle -- this one warps the REAL
    cursor through each waypoint and releases only once the far corner
    is truly reached."""
    dbg.warp_cursor(qmp, x0, y0)
    qmp.mouse_down()
    time.sleep(0.2)
    for i in range(1, steps + 1):
        dbg.warp_cursor(qmp, x0 + (x1 - x0) * i // steps,
                         y0 + (y1 - y0) * i // steps)
    qmp.mouse_up()
    time.sleep(0.3)


def ink_count(png, rect, rgb, tol=14):
    """How many sampled pixels inside `rect` are within `tol` of `rgb`.
    The icons view's folder glyph is (240, 190, 70) -- gen_icons.py's
    icon_folder() -- which no table row ever draws."""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    x, y, w, h = rect
    n = 0
    for yy in range(y, y + h, 2):
        for xx in range(x, x + w, 2):
            px = im.getpixel((xx, yy))
            if all(abs(px[i] - rgb[i]) <= tol for i in range(3)):
                n += 1
    return n


def setup_fixture(dbg):
    # Removed and rebuilt, never reused: `make iso` re-seeds by SYNC, so
    # anything a previous run wrote is still there and a dirty fixture
    # fails in a way that reads exactly like a code regression
    # (CLAUDE.md).
    dbg.send(f"sh rm -r {SRC}")
    dbg.send(f"sh rm -r {DST}")
    dbg.send(f"sh mkdir {SRC}")
    dbg.send(f"sh mkdir {DST}")
    dbg.send(f"sh touch {SRC}/one.txt")
    dbg.send(f"sh touch {SRC}/two.txt")
    dbg.send(f"sh touch {SRC}/three.txt")
    dbg.send(f"sh mkdir {SRC}/sub")


def teardown_fixture(dbg):
    """Leave the image as it was found.

    Not politeness: `make iso` re-seeds disk.img by SYNC, so anything
    left here survives into every later run AND into `check_layout.py`,
    which fails the build on a directory no doc describes. This tool's
    own leftovers turned preflight red once.
    """
    dbg.send(f"sh rm -r {SRC}")
    dbg.send(f"sh rm -r {DST}")


def run(dbg, qmp, tmp, res):
    setup_fixture(dbg)

    win = dbg.spawn(f"{SPAWN_PATH} {SRC} {DST}", TITLE)
    res.check("the File Manager runs as a ring-3 process with its own window",
              win is not None, f"no window titled {TITLE!r}")
    if not win:
        return

    lay = layout_now(dbg, win)
    res.check("it reports two panes", lay is not None and lay.complete(),
              f"layout={lay.pane if lay else None}")
    if not lay:
        return

    # --- 1. the listing agrees with the shell --------------------------
    shell = listing(dbg, SRC)
    # +1 for the synthetic ".." row, which /fmtest has because it is not
    # the root.
    res.check("the left pane lists what `ls` lists",
              lay.rows.get(0) == len(shell) + 1,
              f"pane rows {lay.rows.get(0)}, ls {shell}")
    res.check("each pane opened where it was told",
              lay.dir.get(0) == SRC and lay.dir.get(1) == DST,
              f"dirs {lay.dir}")

    # --- 2. Tab switches the active pane -------------------------------
    dbg.click(*lay.pane_centre(0))
    lay = wait_layout(dbg, win, lambda l: l.active == 0) or lay
    res.check("clicking a pane makes it active", lay.active == 0, f"active {lay.active}")

    dbg.key(K_TAB)
    lay = wait_layout(dbg, win, lambda l: l.active == 1) or lay
    res.check("Tab switches to the other pane", lay.active == 1, f"active {lay.active}")
    dbg.key(K_TAB)
    lay = wait_layout(dbg, win, lambda l: l.active == 0) or lay

    # --- 3. the active pane is MARKED on screen ------------------------
    # Pixels, with the other pane's header as the control: a paint that
    # tinted both would pass "the active header is accent-coloured".
    ax, ay = lay.header_point(0)
    bx, by = lay.header_point(1)
    png = os.path.join(tmp, "fm-active.png")
    qmp.stable_pixels(png)
    from PIL import Image
    im = Image.open(png).convert("RGB")
    active_px, other_px = im.getpixel((ax, ay)), im.getpixel((bx, by))
    res.check("the active pane's path strip is drawn differently from the other's",
              active_px != other_px, f"active {active_px} other {other_px}")

    # --- 4. descend, and come back -------------------------------------
    dbg.key(K_DOWN)   # off the ".." row
    lay = wait_layout(dbg, win, lambda l: l.selected != "-") or lay
    # `sub` sorts among the directories, which lead -- so one step down
    # from ".." is the directory row.
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{SRC}/sub") or lay
    res.check("Enter descends into a directory", lay.dir.get(0) == f"{SRC}/sub",
              f"dir {lay.dir.get(0)}")

    dbg.key(K_BACKSPACE)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC) or lay
    res.check("Backspace goes back up", lay.dir.get(0) == SRC, f"dir {lay.dir.get(0)}")
    res.check("and selects the directory it just left",
              lay.selected == "sub", f"selected {lay.selected!r}")

    # --- 4b. type-ahead --------------------------------------------------
    # The fixture sorts on screen as: .., sub, one.txt, three.txt,
    # two.txt (directories lead). So 't' must reach three.txt and a
    # second 't' must CYCLE to two.txt -- the pair is what tells a
    # working search from one that finds a t and stops. `sys_listdir`
    # makes no promise about the order it hands the entries over in, so
    # a search walking that order rather than the sorted view lands
    # somewhere this cannot predict, which is the point.
    dbg.key(K_HOME)
    lay = wait_layout(dbg, win, lambda l: True) or lay

    dbg.key("t")
    lay = wait_layout(dbg, win, lambda l: l.selected == "three.txt") or lay
    res.check("typing a letter seeks to the first matching row",
              lay.selected == "three.txt", f"selected {lay.selected!r}")

    dbg.key("t")
    lay = wait_layout(dbg, win, lambda l: l.selected == "two.txt") or lay
    res.check("the same letter again cycles to the next match",
              lay.selected == "two.txt", f"selected {lay.selected!r}")

    # A prefix, not just a letter: 'o' alone would reach one.txt anyway,
    # so the check that means something is that 'o' then 'n' does not
    # land on three.txt by way of a search that ignored the second key.
    dbg.key("o")
    dbg.key("n")
    lay = wait_layout(dbg, win, lambda l: l.selected == "one.txt") or lay
    res.check("two letters build a prefix", lay.selected == "one.txt",
              f"selected {lay.selected!r}")

    # --- 5. marking, and copying the marked SET ------------------------
    # Home, then down past `sub` onto the files, and mark two of them.
    # Insert toggles and steps down, so two presses mark two adjacent
    # rows. The Home is not decoration: this phase used to inherit the
    # selection the phase above left, which made it fail the moment a
    # phase was inserted between them.
    dbg.key(K_HOME)
    dbg.key(K_DOWN)
    dbg.key(K_DOWN)
    dbg.key(K_INSERT)
    dbg.key(K_INSERT)
    lay = wait_layout(dbg, win, lambda l: l.marked[0] == 2) or lay
    res.check("Insert marks files, and the app counts them",
              lay.marked[0] == 2, f"marked {lay.marked}")

    # Which two: the pane lists directories first and then files in name
    # order (lib/dirsort.h), so the rows below `sub` are one.txt,
    # three.txt, two.txt -- and the two Inserts above marked the first
    # two of them.
    dbg.key(K_F5)
    names = wait_listing(dbg, DST, lambda n: len(n) >= 2)
    res.check("F5 copies BOTH marked files to the other pane",
              set(names) == {"one.txt", "three.txt"}, f"{DST} holds {names}")
    # The load-bearing half: an app that copied the whole directory
    # would pass the check above and fail this one.
    res.check("and copies ONLY the marked ones", "two.txt" not in names,
              f"{DST} holds {names}")

    # --- 6. delete asks first ------------------------------------------
    dbg.click(*lay.pane_centre(1))          # act on the destination pane
    lay = wait_layout(dbg, win, lambda l: l.active == 1) or lay
    dbg.key(K_DOWN)                          # off ".."
    lay = wait_layout(dbg, win, lambda l: l.selected != "-") or lay
    victim = lay.selected

    dbg.key(K_F8)
    lay = wait_layout(dbg, win, lambda l: l.modal == 1) or lay
    res.check("F8 opens a confirmation rather than deleting", lay.modal == 1,
              f"modal {lay.modal}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.modal == 0) or lay
    still = listing(dbg, DST)
    res.check("Esc cancels it and the file is still there", victim in still,
              f"{victim!r} not in {still}")

    dbg.key(K_F8)
    wait_layout(dbg, win, lambda l: l.modal == 1)
    dbg.key(K_ENTER)
    gone = wait_listing(dbg, DST, lambda n: victim not in n)
    res.check("confirming it deletes the file", victim not in gone,
              f"{victim!r} still in {gone}")

    # --- 7. a new directory --------------------------------------------
    dbg.key(K_F7)
    lay = wait_layout(dbg, win, lambda l: l.modal == 2) or lay
    res.check("F7 asks for a name", lay.modal == 2, f"modal {lay.modal}")
    # settle=True (the default) for every typed character: sent
    # back-to-back with settle=False they outrun the client, and the
    # field commits empty -- which reads exactly like a broken mkdir.
    for ch in "newdir":
        dbg.key(ch)
    dbg.key(K_ENTER)
    made = wait_listing(dbg, DST, lambda n: "newdir" in n)
    res.check("typing a name and pressing Enter creates the directory",
              "newdir" in made, f"{DST} holds {made}")

    # --- 8. rename ------------------------------------------------------
    lay = wait_layout(dbg, win, lambda l: l.selected == "newdir") or lay
    dbg.key(K_F2)
    lay = wait_layout(dbg, win, lambda l: l.modal == 2) or lay
    # The field is pre-filled with the current name, so clear it first.
    for _ in range(8):
        dbg.key(K_BACKSPACE)
    for ch in "renamed":
        dbg.key(ch)
    dbg.key(K_ENTER)
    renamed = wait_listing(dbg, DST, lambda n: "renamed" in n)
    res.check("F2 renames the selection", "renamed" in renamed and "newdir" not in renamed,
              f"{DST} holds {renamed}")

    # --- 9. an association opens the right app --------------------------
    dbg.click(*lay.pane_centre(0))
    lay = wait_layout(dbg, win, lambda l: l.active == 0) or lay
    dbg.key("0x98")  # End -- the last row is a file, whatever the sort
    lay = wait_layout(dbg, win, lambda l: l.selected not in (None, "-", "sub")) or lay
    opened = lay.selected
    dbg.key(K_ENTER)
    # The window's TITLE is the assertion, not its existence: Notepad
    # titles itself after the file it holds, so a launch that ignored the
    # path would show "untitled" and pass a "did a window appear" check.
    deadline = time.time() + 15.0
    titles = []
    while time.time() < deadline:
        titles = [w["title"] for w in dbg.windows()]
        if any(opened in t for t in titles):
            break
        time.sleep(0.3)
    res.check("Enter on a .txt opens it in Notepad, via the .desktop Handles= key",
              any(opened in t for t in titles), f"windows {titles}, expected one holding {opened!r}")

    # --- 10. the panes are remembered -----------------------------------
    # The LEFT pane is the one this run navigated, so it is the one whose
    # saved value can be asserted; requiring `right=` too would be
    # asserting that something got written which nothing asked for.
    conf = dbg.send("sh cat /etc/files.conf") or ""
    res.check("the navigated pane's directory is written to /etc/files.conf",
              f"left={SRC}" in conf, f"conf: {conf!r}")


    # --- 11. the icons view, the rubber band, one pane, and the tree ----
    # A fresh, known fixture: the sections above deleted, created and
    # renamed things, and these checks want a listing they can count.
    dbg.send(f"sh rm -r {SRC}")
    dbg.send(f"sh mkdir {SRC}")
    for n in ("alpha.txt", "bravo.txt", "charlie.txt", "delta.txt", "echo.txt"):
        dbg.send(f"sh touch {SRC}/{n}")
    dbg.send(f"sh mkdir {SRC}/sub")
    # 6 entries + ".." = 7 rows, arriving via the generation poll.
    lay = wait_layout(dbg, win, lambda l: l.rows.get(0) == 7 and
                       l.dir.get(0) == SRC and l.active == 0)
    res.check("the fixture reload arrived (7 rows, left pane active)",
              lay is not None and lay.rows.get(0) == 7 and lay.active == 0,
              f"lay={lay and (lay.rows, lay.dir, lay.active)}")
    if lay is None:
        teardown_fixture(dbg)
        return

    # Section 9 opened Notepad, which sits on top and HOLDS THE FOCUS
    # -- F10 would open ITS menu, and a focusing click would land on it.
    # Close everything that is not the File Manager (`gui close` asks
    # through wm_request_close(), the X button's own path); the FM then
    # tops the stack and takes the focus back.
    ox, oy = win["content"]["x"], win["content"]["y"]
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] != TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.3)
    deadline = time.time() + 8.0
    while time.time() < deadline:
        if all(w2["title"] == TITLE for w2 in dbg.windows()):
            break
        time.sleep(0.3)

    # View -> Icons switches the ACTIVE pane only.
    menu_pick(dbg, K_RIGHT, K_DOWN, K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[0] == 2)
    res.check("View->Icons puts the active pane in icons mode, the other stays",
              lay is not None and lay.view and lay.view[0] == 2 and lay.view[1] == 1,
              f"view={lay and lay.view}")
    if not (lay and lay.view and lay.view[0] == 2):
        teardown_fixture(dbg)
        return

    # The folder glyph's own yellow is on screen in the icons pane and
    # NOT in the details pane -- the control that catches a mode switch
    # that was reported and never drawn.
    png = os.path.join(tmp, "fm_icons.png")
    qmp.stable_pixels(png)
    px, py, pw, ph = lay.pane[0]
    qx, qy, qw, qh = lay.pane[1]
    folder = (240, 190, 70)
    left_ink = ink_count(png, (ox + px, oy + py, pw, ph), folder)
    right_ink = ink_count(png, (ox + qx, oy + qy, qw, qh), folder)
    res.check("folder-icon ink is in the icons pane and not in the details pane",
              left_ink > 20 and right_ink == 0,
              f"left={left_ink} right={right_ink}")

    # The grid takes the keyboard: type-ahead, Enter, Backspace.
    dbg.key("0x73")  # 's' seeks to sub/
    lay = wait_layout(dbg, win, lambda l: l.selected == "sub")
    res.check("type-ahead seeks in the grid", lay is not None and lay.selected == "sub",
              f"selected={lay and lay.selected}")
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{SRC}/sub")
    res.check("Enter descends from the grid",
              lay is not None and lay.dir.get(0) == f"{SRC}/sub", f"dir={lay and lay.dir}")
    dbg.key(K_BACKSPACE)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC)
    res.check("Backspace comes back up",
              lay is not None and lay.dir.get(0) == SRC, f"dir={lay and lay.dir}")

    # The rubber band: sweep from empty space over the grid; the marks
    # are the band's selection. 6 markable entries (".." is not).
    x0, y0 = ox + px + pw - 25, oy + py + ph - 15
    x1, y1 = ox + px + 8, oy + py + 8
    band_drag(dbg, qmp, x0, y0, x1, y1)
    lay = wait_layout(dbg, win, lambda l: l.marked[0] == 6)
    res.check("a rubber-band sweep marks every swept icon",
              lay is not None and lay.marked[0] == 6, f"marked={lay and lay.marked}")
    qmp.click_at(x0, y0)
    lay = wait_layout(dbg, win, lambda l: l.marked[0] == 0)
    res.check("a click on empty space unmarks everything",
              lay is not None and lay.marked[0] == 0, f"marked={lay and lay.marked}")

    # One pane: the active pane fills the width; toggling back restores.
    cw = win["content"]["w"]
    menu_pick(dbg, K_RIGHT, K_DOWN, K_DOWN, K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[2] == 1)
    res.check("View->Second pane collapses to one pane, full width",
              lay is not None and lay.view and lay.view[2] == 1 and
              lay.pane[0][2] > cw * 3 // 4,
              f"view={lay and lay.view} pane0={lay and lay.pane.get(0)}")
    menu_pick(dbg, K_RIGHT, K_DOWN, K_DOWN, K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[2] == 0)
    res.check("toggling again restores the second pane",
              lay is not None and lay.view and lay.view[2] == 0 and
              lay.pane[0][2] < cw * 3 // 4, f"view={lay and lay.view}")

    # The folder tree: a lazy uui_tree over the open set.
    menu_pick(dbg, K_RIGHT, K_DOWN, K_DOWN, K_DOWN, K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 1 and l.treebox)
    res.check("View->Folder tree adds the tree column and the panes move right",
              lay is not None and lay.view and lay.view[3] == 1 and
              lay.view[4] > 1 and lay.pane[0][0] > 0 and lay.treebox is not None,
              f"view={lay and lay.view} pane0={lay and lay.pane.get(0)}")
    if lay and lay.treebox:
        tx, ty, tw, th, trh, tsel = lay.treebox
        res.check("the tree pre-selects the active pane's directory",
                  tsel >= 1, f"selected id={tsel}")

        # Row 1 is "/"'s first child ("bin"). Clicking its LABEL
        # navigates the active pane; clicking its EXPANDER (depth-1
        # triangle at x ~ pad + indent) relists and GROWS the node
        # count without navigating anywhere new.
        n_before = lay.view[4]
        qmp.click_at(ox + tx + tw // 2, oy + ty + trh + trh // 2)
        lay = wait_layout(dbg, win, lambda l: l.dir.get(0) not in (SRC, None))
        res.check("clicking a tree row navigates the active pane",
                  lay is not None and lay.dir.get(0) not in (SRC, None),
                  f"dir={lay and lay.dir}")
        qmp.click_at(ox + tx + 20, oy + ty + trh + trh // 2)
        lay = wait_layout(dbg, win, lambda l: l.view and l.view[4] > n_before)
        res.check("clicking its expander lazily lists the directory's children",
                  lay is not None and lay.view and lay.view[4] > n_before,
                  f"nodes {n_before} -> {lay and lay.view and lay.view[4]}")
        qmp.click_at(ox + tx + 20, oy + ty + trh + trh // 2)
        lay = wait_layout(dbg, win, lambda l: l.view and l.view[4] == n_before)
        res.check("clicking it again collapses back to the open set",
                  lay is not None and lay.view and lay.view[4] == n_before,
                  f"nodes -> {lay and lay.view and lay.view[4]}")

    # The choices persist -- and are then RESET, because a leftover
    # icons/tree state changes what every later run of this tool sees
    # (CLAUDE.md: a test that applies a setting changes the machine).
    conf = dbg.send(f"sh cat {FILES_CONF}") or ""
    res.check("the view choices persist in /etc/files.conf",
              "left_view=icons" in conf and "tree=1" in conf and "panes=2" in conf,
              f"conf: {conf!r}")
    dbg.send(f"sh rm {FILES_CONF}")

    teardown_fixture(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()

    tmp = args.logs or "/tmp"
    os.makedirs(tmp, exist_ok=True)

    res = Result()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, tmp, res)
    finally:
        dbg.close()

    print(f"\nfilemanager_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
