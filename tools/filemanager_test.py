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
PROPERTIES_EXEC = "/bin/wm/apps/properties"
FILES_EXEC = "/bin/wm/apps/files"
# Ctrl+<letter> arrives as the control code (api/keyboard.h).
K_CTRL_C, K_CTRL_X, K_CTRL_V, K_TAB = "0x03", "0x18", "0x16", "0x09"

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
        self.hover = None     # (pane0 hovered row, pane1 hovered row)
        self.view = None      # [mode0, mode1, single, tree_on, tree_nodes]
        self.treebox = None   # [x, y, w, h, row_h, selected_id]
        self.split = None     # (tree fraction, pane fraction), per mille
        self.splitbox = {}    # 0 = tree divider, 1 = pane divider -> [x,y,w,h]
        self.ctx = None       # is the context menu open?
        self.ctxbox = None    # its popup [x, y, w, h]
        self.menu = None      # open level-0 popup [x, y, w, h]
        self.menuhot = None   # (open depth, level-0 hot row)
        self.cellgrid = {}    # pane -> [x0, y0, cell_w, cell_h, cols]
        self.toolbar = None   # the strip [x, y, w, h]
        self.tbitems = {}     # item index -> [x, y, w, h]
        self.active = None
        self.selected = None
        self.modal = None
        for line in lines:
            if "files: layout " not in line:
                continue
            p = line.split("files: layout ", 1)[1].split()
            if not p:
                continue
            try:
                self._field(p)
            except ValueError:
                # A TORN LINE, NOT A BROKEN APP. The app's klog and the
                # serial console's own echo share one stream, so a
                # command sent while the app is drawing can land in the
                # middle of a report line -- and an unguarded int() then
                # takes the whole tool down with a traceback that names
                # the parser rather than the race. Skipping costs one
                # frame; the next report is along in milliseconds.
                continue

    def _field(self, p):
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
        elif p[0] == "hover" and len(p) >= 3:
            self.hover = (int(p[1]), int(p[2]))
        elif p[0] == "view" and len(p) >= 8:
            # "view <mode0> <mode1> single <s> tree <t> <nodes>"
            self.view = [int(p[1]), int(p[2]), int(p[4]),
                         int(p[6]), int(p[7])]
        elif p[0] == "treebox" and len(p) >= 7:
            self.treebox = [int(v) for v in p[1:7]]
        elif p[0] == "ctx" and len(p) >= 2:
            self.ctx = int(p[1])
        elif p[0] == "ctxbox" and len(p) >= 5:
            self.ctxbox = [int(v) for v in p[1:5]]
        elif p[0] == "split" and len(p) >= 3:
            self.split = (int(p[1]), int(p[2]))
        elif p[0] == "splitbox" and len(p) >= 6:
            self.splitbox[int(p[1])] = [int(v) for v in p[2:6]]
        elif p[0] == "menu" and len(p) >= 5:
            self.menu = [int(v) for v in p[1:5]]
        elif p[0] == "menuhot" and len(p) >= 3:
            self.menuhot = (int(p[1]), int(p[2]))
        elif p[0] == "cellgrid" and len(p) >= 7:
            self.cellgrid[int(p[1])] = [int(v) for v in p[2:7]]
        elif p[0] == "toolbar" and len(p) >= 5:
            self.toolbar = [int(v) for v in p[1:5]]
        elif p[0] == "tbitem" and len(p) >= 6:
            self.tbitems[int(p[1])] = [int(v) for v in p[2:6]]

    def complete(self):
        return 0 in self.pane and 1 in self.pane and self.active is not None

    def pane_centre(self, i):
        x, y, w, h = self.pane[i]
        return (self.ox + x + w // 2, self.oy + y + h // 2)

    def split_point(self, i):
        """A pixel in the middle of divider i's band (0 = tree, 1 = panes)."""
        x, y, w, h = self.splitbox[i]
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


# A POLL MUST ACCUMULATE, NOT RE-READ. The app emits its report as ONE
# BLOCK and only when the block CHANGES (uapp.c's layout_log_flush), so
# an idle window emits nothing at all -- and a serial read that lands
# mid-block hands back a frame missing everything after the split. Parse
# each read on its own and that frame is the only one you will ever get:
# the poll then spins until it times out while the app sits there having
# already reported exactly what was asked for. Accumulating costs
# nothing, because Layout() already keeps only the last complete frame.
_LAST_BUF = []


def _collect(dbg, buf):
    buf.extend(dbg.logs("files:", clear=True))
    _LAST_BUF[:] = buf
    return buf


def layout_now(dbg, win, tries=25):
    """The app's CURRENT layout report, polled rather than slept for."""
    buf = []
    for _ in range(tries):
        # NOT cleared first: the app draws when something happens, so
        # the report we want may already be in the buffer -- an
        # unconditional clear here threw away the only frame the app had
        # logged and reported a working window as silent.
        if _collect(dbg, buf):
            lay = Layout(win["content"], buf)
            if lay.complete():
                return lay
        time.sleep(0.2)
    return None


def wait_layout(dbg, win, pred, timeout=12.0):
    """Poll the app's own report until `pred` holds. Waiting on the
    OBSERVABLE rather than on a fixed sleep (CLAUDE.md)."""
    deadline = time.time() + timeout
    last = None
    buf = []
    while time.time() < deadline:
        if _collect(dbg, buf):
            lay = Layout(win["content"], buf)
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


def sure_click(dbg, qmp, x, y):
    """warp_cursor + click: closed-loop, where click_at()'s open-loop
    goto can land a long accelerated journey somewhere else entirely
    and silently desync its own position tracking."""
    dbg.warp_cursor(qmp, x, y)
    qmp.click()


def sure_rclick(dbg, qmp, x, y):
    """A SECONDARY click with a confirmed cursor, the mirror of
    sure_click(). Right-click is what opens a context menu, and the app
    reads the position off the event -- so landing somewhere else opens
    the menu on the wrong row, which reads as the menu being wrong."""
    dbg.warp_cursor(qmp, x, y)
    qmp.click(button="right")
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


def gutter_ink(png, menu_rect, ox, oy):
    """Dark pixels in the popup's tick GUTTER (the two-character column
    left of the labels, uui_menubar.c's gutter()). Labels start after
    it, so the only ink here is UUI_MI_CHECKED's tick."""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    mx, my, mw, mh = menu_rect
    n = 0
    for yy in range(oy + my + 2, oy + my + mh - 2):
        for xx in range(ox + mx + 4, ox + mx + 18):
            r, g, b = im.getpixel((xx, yy))
            if r + g + b < 300:
                n += 1
    return n


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
    deadline = time.time() + 8.0
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
    sure_click(dbg, qmp, x0, y0)
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
        sure_click(dbg, qmp, ox + tx + tw // 2, oy + ty + trh + trh // 2)
        lay = wait_layout(dbg, win, lambda l: l.dir.get(0) not in (SRC, None))
        res.check("clicking a tree row navigates the active pane",
                  lay is not None and lay.dir.get(0) not in (SRC, None),
                  f"dir={lay and lay.dir}")
        sure_click(dbg, qmp, ox + tx + 20, oy + ty + trh + trh // 2)
        lay = wait_layout(dbg, win, lambda l: l.view and l.view[4] > n_before)
        res.check("clicking its expander lazily lists the directory's children",
                  lay is not None and lay.view and lay.view[4] > n_before,
                  f"nodes {n_before} -> {lay and lay.view and lay.view[4]}")
        sure_click(dbg, qmp, ox + tx + 20, oy + ty + trh + trh // 2)
        lay = wait_layout(dbg, win, lambda l: l.view and l.view[4] == n_before)
        res.check("clicking it again collapses back to the open set",
                  lay is not None and lay.view and lay.view[4] == n_before,
                  f"nodes -> {lay and lay.view and lay.view[4]}")

    # --- 11b. the resizable dividers ------------------------------------
    # Tree on, two panes: all three columns and both dividers are up.
    # A broken splitter still lets everything else here pass, so every
    # check below names a NEIGHBOUR that must not move -- half the
    # assertion (CLAUDE.md).
    lay = wait_layout(dbg, win, lambda l: len(l.splitbox) == 2) or lay
    ok_geom = False
    if lay and len(lay.splitbox) == 2 and lay.treebox:
        tb = lay.treebox
        s0, s1 = lay.splitbox[0], lay.splitbox[1]
        ok_geom = (tb[0] + tb[2] == s0[0] and
                   lay.pane[0][0] == s0[0] + s0[2] and
                   lay.pane[0][0] + lay.pane[0][2] == s1[0] and
                   lay.pane[1][0] == s1[0] + s1[2])
    res.check("both dividers sit exactly between the columns they divide",
              ok_geom, f"tree={lay and lay.treebox} splits={lay and lay.splitbox} "
                       f"panes={lay and lay.pane}")

    if ok_geom:
        # 1. Dragging the PANE divider right widens the left pane and
        #    narrows the right one, and leaves the tree alone.
        tw_before = lay.treebox[2]
        w0, w1 = lay.pane[0][2], lay.pane[1][2]
        sx, sy = lay.split_point(1)
        band_drag(dbg, qmp, sx, sy, sx + 60, sy)
        lay = wait_layout(dbg, win, lambda l: 0 in l.pane and l.pane[0][2] != w0) or lay
        res.check("dragging the pane divider right widens the left pane, "
                  "narrows the right, and leaves the tree put",
                  lay.pane[0][2] > w0 + 30 and lay.pane[1][2] < w1 - 30 and
                  lay.treebox and lay.treebox[2] == tw_before,
                  f"panes {w0}/{w1} -> {lay.pane[0][2]}/{lay.pane[1][2]}, "
                  f"tree {tw_before} -> {lay.treebox and lay.treebox[2]}")

        # 2. The TREE divider moves the tree and BOTH panes, together.
        tw = lay.treebox[2]
        px0 = lay.pane[0][0]
        sx, sy = lay.split_point(0)
        band_drag(dbg, qmp, sx, sy, sx + 50, sy)
        lay = wait_layout(dbg, win,
                           lambda l: l.treebox and l.treebox[2] != tw) or lay
        res.check("dragging the tree divider widens the tree and moves the panes over",
                  lay.treebox[2] > tw + 20 and lay.pane[0][0] > px0 + 20,
                  f"tree {tw} -> {lay.treebox[2]}, pane0 x {px0} -> {lay.pane[0][0]}")

        # 3. THE MINIMUM HOLDS. Dragged far past its own left edge, the
        #    left pane keeps a usable width instead of collapsing to
        #    nothing with no handle left to drag back.
        sx, sy = lay.split_point(1)
        band_drag(dbg, qmp, sx, sy, sx - 900, sy, steps=6)
        lay = wait_layout(dbg, win, lambda l: 0 in l.pane) or lay
        res.check("a divider dragged off the edge stops at the pane minimum",
                  lay.pane[0][2] >= 8 and lay.pane[0][2] < 120,
                  f"left pane clamped to {lay.pane[0][2]}px")

        # 4. Double click puts it back to the middle -- and the tree
        #    divider, which was NOT double-clicked, must stay where it
        #    was dragged.
        tw = lay.treebox[2]
        sx, sy = lay.split_point(1)
        dbg.warp_cursor(qmp, sx, sy)
        qmp.click()
        time.sleep(0.15)
        qmp.click()
        lay = wait_layout(dbg, win,
                           lambda l: l.split and abs(l.split[1] - 500) < 3) or lay
        res.check("double-clicking a divider resets only that one",
                  lay.split and abs(lay.split[1] - 500) < 3 and
                  lay.treebox and lay.treebox[2] == tw,
                  f"split={lay.split} tree {tw} -> {lay.treebox and lay.treebox[2]}")

        # 5. The keyboard: Ctrl+Right nudges the pane divider one column.
        frac = lay.split[1]
        w0 = lay.pane[0][2]
        dbg.key(K_RIGHT, mods="ctrl")
        lay = wait_layout(dbg, win, lambda l: l.split and l.split[1] != frac) or lay
        res.check("Ctrl+Right nudges the pane divider without the mouse",
                  lay.split and lay.split[1] > frac and lay.pane[0][2] > w0,
                  f"frac {frac} -> {lay.split and lay.split[1]}, "
                  f"pane0 {w0} -> {lay.pane[0][2]}")

        # 6. The pointer over the band is the RESIZE cursor -- the
        #    WIN_CURSOR_RESIZE_H the client asked for, resolved by the
        #    compositor. The control is a point inside the pane, which
        #    must stay the plain arrow.
        sx, sy = lay.split_point(1)
        dbg.warp_cursor(qmp, sx, sy)
        time.sleep(0.3)
        on_band = dbg.cursor_shape()
        px, py = lay.pane_centre(1)
        dbg.warp_cursor(qmp, px, py)
        time.sleep(0.3)
        off_band = dbg.cursor_shape()
        res.check("the pointer over a divider is the resize cursor, and not beside it",
                  on_band == DebugConsole.CURSOR_H and
                  off_band == DebugConsole.CURSOR_NORMAL,
                  f"on band={on_band} off band={off_band}")

        # 7. Both positions are remembered, as fractions.
        conf = dbg.send(f"sh cat {FILES_CONF}") or ""
        res.check("both divider positions persist in /etc/files.conf",
                  "pane_split=" in conf and "tree_split=" in conf, f"conf: {conf!r}")

    # The choices persist -- and are then RESET, because a leftover
    # icons/tree state changes what every later run of this tool sees
    # (CLAUDE.md: a test that applies a setting changes the machine).
    conf = dbg.send(f"sh cat {FILES_CONF}") or ""
    res.check("the view choices persist in /etc/files.conf",
              "left_view=icons" in conf and "tree=1" in conf and "panes=2" in conf,
              f"conf: {conf!r}")

    # --- 12. the View menu ticks its active options ---------------------
    # State right now: left pane icons, two panes, tree on -- three of
    # the four View items are checked. The FILE menu is the control (no
    # checkable item in it), and toggling the tree off must take exactly
    # its tick away.
    # Park the cursor far from where the popups drop: its sprite's dark
    # outline reads as tick ink if it is left over the gutter (it was,
    # from the expander click -- 18 phantom pixels).
    qx2, qy2, qw2, qh2 = lay.pane[1] if 1 in lay.pane else lay.pane[0]
    dbg.warp_cursor(qmp, ox + qx2 + qw2 - 20, oy + qy2 + qh2 - 20)

    dbg.key(K_F10)  # File menu opens
    lay = wait_layout(dbg, win, lambda l: l.menu is not None)
    n_file = -1
    if lay and lay.menu:
        png = os.path.join(tmp, "fm_menu_file.png")
        qmp.stable_pixels(png)
        n_file = gutter_ink(png, lay.menu, ox, oy)
    res.check("control: the File menu's tick gutter is empty",
              n_file == 0, f"gutter ink={n_file}")

    dbg.key(K_RIGHT)  # View menu
    time.sleep(0.4)
    lay = wait_layout(dbg, win, lambda l: l.menu is not None)
    n_on = -1
    if lay and lay.menu:
        png = os.path.join(tmp, "fm_menu_view_on.png")
        qmp.stable_pixels(png)
        n_on = gutter_ink(png, lay.menu, ox, oy)
    res.check("the View menu draws ticks on its active options",
              n_on > 0, f"gutter ink={n_on}")

    # An open popup owns the POINTER, not just the click: hovering a
    # menu row over the icons grid must light no cell beneath (the
    # highlight was visible past the popup's edge), and the active
    # pane's accent outline must stay UNDER the popup -- it used to be
    # drawn from on_draw_over, which runs after the overlay pass, so
    # two accent lines crossed the menu. Both found by the maintainer
    # in one screenshot.
    menu_rect = lay.menu if lay else None
    tree_rect = lay.treebox if lay else None
    if menu_rect:
        mx, my, mw, mh = menu_rect
        # Warping over a row moves the menu's hot row, which is logged
        # (menuhot) -- the pane hover staying put must not be proven by
        # silence, since the dedup logs nothing for an unchanged frame.
        dbg.warp_cursor(qmp, ox + mx + mw // 2, oy + my + mh * 3 // 4)
        lay = wait_layout(dbg, win,
                           lambda l: l.menuhot is not None and l.menuhot[0] > 0
                           and l.hover is not None)
        res.check("hovering a menu row lights nothing under the popup",
                  lay is not None and lay.hover == (-1, -1),
                  f"hover={lay and lay.hover} menuhot={lay and lay.menuhot}")
        png = os.path.join(tmp, "fm_menu_over_pane.png")
        qmp.stable_pixels(png)
        accent = (70, 110, 160)  # utheme.c's accent, the outline's ink
        inside = ink_count(png, (ox + mx + 2, oy + my + 2, mw - 4, mh - 4),
                            accent, tol=8)
        outside = ink_count(png, (ox, oy, win["content"]["w"],
                                   win["content"]["h"]), accent, tol=8)
        res.check("the active-pane outline stays under the popup",
                  inside == 0 and outside > 0,
                  f"accent inside popup={inside}, in window={outside}")

    # A menu click must NOT fall through to the folder tree under the
    # popup (the hand-routed-menubar bug, found by the maintainer: a
    # click on a View item also selected the tree row beneath it). The
    # View popup overlaps the tree column, so click its first row --
    # "Details", a harmless commit -- and require the tree's selection
    # and the pane's directory to stay put while the commit LANDS.
    if menu_rect and tree_rect:
        lay2 = wait_layout(dbg, win, lambda l: True) or lay
        dir_before = lay2.dir.get(0) if lay2 else None
        sel_before = tree_rect[5]
        mx, my = menu_rect[0], menu_rect[1]
        sure_click(dbg, qmp, ox + mx + 20, oy + my + 10)
        lay = wait_layout(dbg, win, lambda l: l.view and l.view[0] == 1)
        res.check("a click on a menu item commits it and does NOT reach the tree",
                  lay is not None and lay.view and lay.view[0] == 1 and
                  lay.dir.get(0) == dir_before and
                  (lay.treebox is None or lay.treebox[5] == sel_before),
                  f"view={lay and lay.view} dir={lay and lay.dir.get(0)} "
                  f"tree sel {sel_before} -> {lay and lay.treebox and lay.treebox[5]}")
    else:
        res.check("a click on a menu item commits it and does NOT reach the tree",
                  False, "no menu/tree geometry to aim with")
        dbg.key(K_ESC)
        dbg.key(K_ESC)

    # The commit above CLOSED the menu, so the toggle is a full pick.
    menu_pick(dbg, K_RIGHT, K_DOWN, K_DOWN, K_DOWN, K_ENTER)  # tree off
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 0)
    res.check("(the tree toggle landed)", lay is not None and lay.view and
              lay.view[3] == 0, f"view={lay and lay.view}")
    dbg.warp_cursor(qmp, ox + qx2 + qw2 - 20, oy + qy2 + qh2 - 20)
    dbg.key(K_F10)
    time.sleep(0.2)
    dbg.key(K_RIGHT)
    time.sleep(0.4)
    lay = wait_layout(dbg, win, lambda l: l.menu is not None)
    n_off = -1
    if lay and lay.menu:
        png = os.path.join(tmp, "fm_menu_view_off.png")
        qmp.stable_pixels(png)
        n_off = gutter_ink(png, lay.menu, ox, oy)
    res.check("turning the folder tree off takes its tick away, keeping the others",
              0 < n_off < n_on, f"gutter ink {n_on} -> {n_off}")
    dbg.key(K_ESC)
    dbg.key(K_ESC)


    # --- 13. the toolbar ------------------------------------------------
    # Same commands, same item_flags as the menus: Up/Refresh, then the
    # four View toggles drawn LATCHED when their thing is on.
    # Fourteen: Up, Refresh, sep, the five VERBS that used to be a
    # button row across the bottom, sep, Details, Icons, sep, Second
    # pane, Folder tree. Separators are indexed too.
    res.check("the toolbar reports its strip and all fourteen items",
              lay is not None and lay.toolbar is not None and len(lay.tbitems) == 14,
              f"toolbar={lay and lay.toolbar} items={lay and sorted(lay.tbitems)}")
    if not (lay and lay.toolbar and len(lay.tbitems) == 14):
        dbg.send(f"sh rm {FILES_CONF}")
        teardown_fixture(dbg)
        return

    def tb_centre(i):
        x, y, w, h = lay.tbitems[i]
        return (ox + x + w // 2, oy + y + h // 2)

    # Up climbs to the parent; at the root it is DISABLED and the click
    # lands on nothing.
    dir_now = lay.dir.get(0)
    parent = "/" if dir_now.count("/") <= 1 else dir_now.rsplit("/", 1)[0]
    sure_click(dbg, qmp, *tb_centre(0))
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == parent)
    res.check("the Up button climbs to the parent directory",
              lay is not None and lay.dir.get(0) == parent,
              f"dir {dir_now} -> {lay and lay.dir.get(0)} (wanted {parent})")
    while lay and lay.dir.get(0) not in (None, "/"):
        sure_click(dbg, qmp, *tb_centre(0))
        nxt = wait_layout(dbg, win, lambda l: l.dir.get(0) != lay.dir.get(0), timeout=5)
        if nxt is None or nxt.dir.get(0) == lay.dir.get(0):
            break
        lay = nxt
    sure_click(dbg, qmp, *tb_centre(0))  # at "/": disabled, must do nothing
    time.sleep(0.8)
    lay = wait_layout(dbg, win, lambda l: True) or lay
    res.check("at the root the Up button is disabled and does nothing",
              lay is not None and lay.dir.get(0) == "/", f"dir={lay and lay.dir.get(0)}")

    # The Folder-tree button toggles the same state the menu ticks, and
    # LATCHES: its background moves to the pressed wash while a sibling
    # stays put (half the assertion is the neighbour, CLAUDE.md).
    sure_click(dbg, qmp, *tb_centre(13))
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 1)
    res.check("the Folder-tree button toggles the tree on",
              lay is not None and lay.view and lay.view[3] == 1, f"view={lay and lay.view}")

    park = (ox + lay.pane[1][0] + lay.pane[1][2] - 20,
            oy + lay.pane[1][1] + lay.pane[1][3] - 20)
    dbg.warp_cursor(qmp, *park)
    png_on = os.path.join(tmp, "fm_tb_latched.png")
    qmp.stable_pixels(png_on)
    from PIL import Image
    im = Image.open(png_on).convert("RGB")
    tx7, ty7 = lay.tbitems[13][0] + 2, lay.tbitems[13][1] + 2
    tx1, ty1 = lay.tbitems[1][0] + 2, lay.tbitems[1][1] + 2
    p7 = im.getpixel((ox + tx7, oy + ty7))
    p1 = im.getpixel((ox + tx1, oy + ty1))
    res.check("the latched button's background differs from its resting sibling's",
              p7 != p1, f"tree btn {p7} vs refresh btn {p1}")

    sure_click(dbg, qmp, *tb_centre(13))
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 0)
    res.check("clicking it again toggles the tree off",
              lay is not None and lay.view and lay.view[3] == 0, f"view={lay and lay.view}")

    # The tooltip: park elsewhere (no cream in the strip's shadow), then
    # hover Refresh past the delay -- it rides the app's tick, so give it
    # delay + one tick. The tip's cream (255, 252, 220) is a colour
    # nothing else in this window draws.
    dbg.warp_cursor(qmp, *park)
    time.sleep(0.6)
    bx, by, bw, bh = lay.tbitems[1]
    tip_rect = (ox + bx, oy + by + bh, 120, 30)
    png0 = os.path.join(tmp, "fm_tip_before.png")
    qmp.stable_pixels(png0)
    n_before = ink_count(png0, tip_rect, (255, 252, 220), tol=6)
    dbg.warp_cursor(qmp, ox + bx + bw // 2, oy + by + bh // 2)
    time.sleep(1.4)
    png1 = os.path.join(tmp, "fm_tip_after.png")
    qmp.stable_pixels(png1)
    n_after = ink_count(png1, tip_rect, (255, 252, 220), tol=6)
    res.check("hovering a button shows its tooltip, and only then",
              n_before == 0 and n_after > 20, f"cream {n_before} -> {n_after}")
    dbg.warp_cursor(qmp, *park)


    # --- 13b. the verbs moved to the toolbar ----------------------------
    # They were five buttons across the bottom. The proof they really
    # moved is not that the strip has more items -- it is that the
    # commands WORK from up here: New folder must open the same prompt
    # F7 opens, and Delete the same confirm F8 opens.
    lay = wait_layout(dbg, win, lambda l: len(l.tbitems) == 14) or lay
    sure_click(dbg, qmp, *tb_centre(5))          # New folder
    lay = wait_layout(dbg, win, lambda l: l.modal not in (None, 0)) or lay
    res.check("the toolbar's New folder opens the same prompt F7 does",
              lay.modal not in (None, 0), f"modal={lay and lay.modal}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.modal == 0) or lay

    # Delete needs something selected, or it complains instead of asking.
    sure_click(dbg, qmp, *lay.pane_centre(0))
    lay = wait_layout(dbg, win, lambda l: True) or lay
    sure_click(dbg, qmp, *tb_centre(7))          # Delete
    lay = wait_layout(dbg, win, lambda l: l.modal not in (None, 0)) or lay
    res.check("the toolbar's Delete opens the same confirm F8 does",
              lay.modal not in (None, 0), f"modal={lay and lay.modal}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.modal == 0) or lay
    res.check("(Esc dismissed it without deleting anything)",
              lay is not None and lay.modal == 0, f"modal={lay and lay.modal}")

    # --- 14. the context menu, and Properties ---------------------------
    # A SECONDARY click inside a pane. Two things a broken version would
    # still pass if they were not both asserted: that the menu opened at
    # all, and that it opened on the row that was pointed AT -- a menu
    # acting on some other row is how a file manager deletes the wrong
    # file.
    lay = wait_layout(dbg, win, lambda l: 0 in l.pane and l.rows.get(0, 0) > 2) or lay
    px, py, pw, _ = lay.pane[0]
    row_h = lay.treebox[4] if lay.treebox else 16
    # Row 2 of the listing: past "..", and not whatever is selected now.
    target = (ox + px + pw // 2, oy + py + row_h * 2 + row_h // 2)
    before_sel = lay.selected
    sure_rclick(dbg, qmp, *target)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 1 and l.ctxbox) or lay
    tail = [ln.strip() for ln in _LAST_BUF][-14:]
    res.check("a right-click inside a pane opens a context menu",
              lay.ctx == 1 and lay.ctxbox is not None,
              f"ctx={lay and lay.ctx} box={lay and lay.ctxbox} tail={tail}")
    res.check("...and it selects the row it was pointed at first",
              lay.selected not in (None, "-") and lay.selected != before_sel,
              f"selected {before_sel} -> {lay and lay.selected}")

    # The popup is placed AT the cursor, not at some fixed corner.
    ok_place = False
    if lay.ctxbox:
        cbx, cby, cbw, cbh = lay.ctxbox
        ok_place = (abs((ox + cbx) - target[0]) <= 6 and
                    (oy + cby) >= target[1] - 2 and cbw > 0 and cbh > 0)
    res.check("the popup opens at the pointer",
              ok_place, f"box={lay and lay.ctxbox} click={target} origin={(ox, oy)}")

    # Esc closes it, and closing must not commit anything -- the same
    # rule every menu here follows.
    dir_before = lay.dir.get(0)
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 0) or lay
    res.check("Esc closes the context menu without committing",
              lay.ctx == 0 and lay.dir.get(0) == dir_before,
              f"ctx={lay and lay.ctx} dir={lay and lay.dir.get(0)}")

    # Properties: the LAST row of the popup, and it opens a WINDOW.
    sel_name = None
    sure_rclick(dbg, qmp, *target)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 1 and l.ctxbox) or lay
    sel_name = lay.selected
    dbg.logs("properties:", clear=True)
    if lay.ctxbox:
        cbx, cby, cbw, cbh = lay.ctxbox
        # The bottom row of the popup. Two pixels in from the border, so
        # a row height this test does not know cannot put it on the edge.
        sure_click(dbg, qmp, ox + cbx + cbw // 2, oy + cby + cbh - 4)
    deadline = time.time() + 8.0
    titles = []
    while time.time() < deadline:
        titles = [w2["title"] for w2 in dbg.windows()]
        if any("Properties" in t for t in titles):
            break
        time.sleep(0.3)
    res.check("Properties opens a window of its own, named for the file",
              any("Properties" in t for t in titles) and
              any(sel_name and sel_name in t for t in titles),
              f"selected={sel_name} windows={titles}")

    # WHAT IT SAYS, read from the app's own report rather than from
    # pixels. A window that opens and shows nothing would pass the check
    # above on its own.
    rows = {}
    deadline = time.time() + 10.0
    while time.time() < deadline:
        for ln in dbg.logs("properties: row", clear=True):
            body = ln.split("properties: row ", 1)[1].strip()
            if "=" in body:
                k, v = body.split("=", 1)
                rows[k] = v
        if "Inode" in rows:
            break
        time.sleep(0.4)
    res.check("Properties reports name, type, location, size and both times",
              all(k in rows for k in ("Name", "Type", "Location", "Size",
                                       "Created", "Modified", "Inode")),
              f"rows={rows}")
    res.check("...with the name it was opened for, and a size in bytes",
              rows.get("Name") == sel_name and "bytes" in rows.get("Size", ""),
              f"Name={rows.get('Name')!r} (wanted {sel_name!r}) Size={rows.get('Size')!r}")

    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if "Properties" in w2["title"]:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.3)

    # A FOLDER counts what is in it -- the recursive walk, which is the
    # reason this is a process and not a modal.
    dbg.send(f"sh mkdir {SRC}/sub")
    dbg.send(f"sh cp /etc/toyos.conf {SRC}/sub/one.conf")
    wait_listing(dbg, f"{SRC}/sub", lambda names: "one.conf" in names)
    dbg.logs("properties:", clear=True)
    dbg.send(f"gui spawn {PROPERTIES_EXEC} {SRC}")
    folder = {}
    deadline = time.time() + 10.0
    while time.time() < deadline:
        for ln in dbg.logs("properties: row", clear=True):
            body = ln.split("properties: row ", 1)[1].strip()
            if "=" in body:
                k, v = body.split("=", 1)
                folder[k] = v
        if folder.get("Contains") and "counting" not in folder.get("Contains", ""):
            break
        time.sleep(0.5)
    res.check("a folder's Properties totals its contents recursively",
              folder.get("Type") == "Folder" and
              "file" in folder.get("Contains", "") and
              "folder" in folder.get("Contains", ""),
              f"Type={folder.get('Type')!r} Contains={folder.get('Contains')!r} "
              f"Size={folder.get('Size')!r}")
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if "Properties" in w2["title"]:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.3)

    # NOTHING LEFT OPEN. An open popup swallows every key, so a context
    # menu still up here makes the sections below type into it -- which
    # is how the type-ahead that navigates to the image fixture landed
    # in /tests and reported the icons view as broken.
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 0) or lay
    res.check("nothing is left open for the sections after this one",
              lay is not None and lay.ctx == 0 and lay.modal == 0,
              f"ctx={lay and lay.ctx} modal={lay and lay.modal}")

    # --- 14b. the clipboard: Ctrl+C / Ctrl+X / Ctrl+V -------------------
    #
    # Driven by KEYS and checked with `ls` -- a different reader from the
    # app that did the work, so a broken copy cannot make the two agree.
    # The cut is the one worth care: it must move NOTHING until the
    # paste, which is the half of Explorer's behaviour a naive
    # implementation gets wrong by moving on Ctrl+X.
    CLIP = "/cliptest"
    dbg.send(f"sh rm -r {CLIP}")
    dbg.send(f"sh mkdir {CLIP}")
    dbg.send(f"sh mkdir {CLIP}/dst")
    dbg.send(f"sh cp /etc/timezones {CLIP}/one.conf")
    wait_listing(dbg, CLIP, lambda names: "one.conf" in names)

    def names(path):
        return sorted(listing(dbg, path))

    # Both panes where the test needs them, by relaunching the app with
    # its two directories as arguments -- an argument is a statement
    # about that launch and does not disturb the saved pair.
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] == TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.4)
    dbg.send(f"gui spawn {FILES_EXEC} {CLIP} {CLIP}/dst")
    win = None
    deadline = time.time() + 15.0
    while time.time() < deadline and not win:
        win = dbg.window(TITLE)
        if not win:
            time.sleep(0.3)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == CLIP) or lay
    res.check("(the panes reopened on the clipboard fixture)",
              lay is not None and lay.dir.get(0) == CLIP and
              lay.dir.get(1) == f"{CLIP}/dst", f"dirs={lay and lay.dir}")

    dbg.key("0x6f")                                  # 'o' seeks one.conf
    lay = wait_layout(dbg, win, lambda l: l.selected == "one.conf") or lay
    dbg.key(K_CTRL_C)
    dbg.key(K_TAB)
    lay = wait_layout(dbg, win, lambda l: l.active == 1) or lay
    dbg.key(K_CTRL_V)
    got = wait_listing(dbg, f"{CLIP}/dst", lambda n: "one.conf" in n)
    res.check("Ctrl+C then Ctrl+V copies into the ACTIVE pane",
              "one.conf" in got, f"dst={got}")
    res.check("...and a copy leaves the original where it was",
              "one.conf" in names(CLIP), f"src={names(CLIP)}")

    # The cut. Back to the left pane, cut, and check NOTHING moved yet.
    dbg.send(f"sh rm {CLIP}/dst/one.conf")
    wait_listing(dbg, f"{CLIP}/dst", lambda n: "one.conf" not in n)
    dbg.key(K_TAB)
    lay = wait_layout(dbg, win, lambda l: l.active == 0) or lay
    dbg.key("0x6f")
    lay = wait_layout(dbg, win, lambda l: l.selected == "one.conf") or lay
    dbg.key(K_CTRL_X)
    time.sleep(1.0)
    res.check("Ctrl+X moves NOTHING until the paste",
              "one.conf" in names(CLIP) and "one.conf" not in names(f"{CLIP}/dst"),
              f"src={names(CLIP)} dst={names(f'{CLIP}/dst')}")

    dbg.key(K_TAB)
    lay = wait_layout(dbg, win, lambda l: l.active == 1) or lay
    dbg.key(K_CTRL_V)
    got = wait_listing(dbg, f"{CLIP}/dst", lambda n: "one.conf" in n)
    res.check("...and the paste then MOVES it, source and all",
              "one.conf" in got and "one.conf" not in names(CLIP),
              f"dst={got} src={names(CLIP)}")

    # A cut is SPENT by its paste: the files are no longer where the
    # clipboard says they are, so a second paste must do nothing rather
    # than fail on every entry.
    dbg.key(K_CTRL_V)
    time.sleep(1.5)
    res.check("a cut is spent by its paste -- a second one does nothing",
              names(f"{CLIP}/dst") == ["one.conf"] and names(CLIP) == ["dst"],
              f"dst={names(f'{CLIP}/dst')} src={names(CLIP)}")

    dbg.send(f"sh rm -r {CLIP}")

    # HANDED BACK AS IT WAS FOUND. This section relaunched the app on its
    # own two directories and left the RIGHT pane active; the sections
    # below navigate by type-ahead from the root and act on the active
    # pane, so leaving either changed makes them fail as if the app were
    # broken (it did -- View->Icons landed on the wrong pane).
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] == TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.4)
    dbg.send(f"gui spawn {FILES_EXEC} / /")
    win = None
    deadline = time.time() + 15.0
    while time.time() < deadline and not win:
        win = dbg.window(TITLE)
        if not win:
            time.sleep(0.3)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == "/" and l.active == 0) or lay
    res.check("(the app is back at the root with the left pane active)",
              lay is not None and lay.dir.get(0) == "/" and lay.active == 0,
              f"dirs={lay and lay.dir} active={lay and lay.active}")

    # --- 15. thumbnails in the icons view -------------------------------
    # A QOI and a JPEG show their own pixels; a text file keeps the
    # generic glyph. Decoding is LAZY (a couple per tick), so the checks
    # POLL for the thumbnail's ink rather than expecting it in frame one.
    dbg.send(f"sh rm -r {SRC}")
    dbg.send(f"sh mkdir {SRC}")
    dbg.send(f"sh cp /usr/share/icons/doom.qoi {SRC}/game.qoi")
    dbg.send(f"sh cp /usr/share/wallpapers/dusk.jpg {SRC}/photo.jpg")
    dbg.send(f"sh touch {SRC}/readme.txt")
    wait_listing(dbg, SRC, lambda names: "readme.txt" in names)

    # Navigate the left pane there (type-ahead: f, m, t seeks fmtest
    # over fmdest) and switch it to icons.
    for c in ("0x66", "0x6d", "0x74"):
        dbg.key(c)
        time.sleep(0.15)
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC)
    menu_pick(dbg, K_RIGHT, K_DOWN, K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[0] == 2 and
                       0 in l.cellgrid)
    res.check("the pane is in icons mode over the image fixture",
              lay is not None and lay.view and lay.view[0] == 2 and
              lay.dir.get(0) == SRC and 0 in lay.cellgrid,
              f"view={lay and lay.view} dir={lay and lay.dir.get(0)}")

    if lay and 0 in lay.cellgrid:
        gx, gy, gcw, gch, gcols = lay.cellgrid[0]

        def cell_rect(view):
            # Icon-box portion of the cell only (the label row excluded).
            return (ox + gx + (view % gcols) * gcw,
                    oy + gy + (view // gcols) * gch, gcw, gch - 14)

        # Rows: ..(0) game.qoi(1) photo.jpg(2) readme.txt(3).
        doom_red = (166, 42, 38)  # the doom icon's plate, its dominant ink
        deadline = time.time() + 8.0
        red = 0
        png = os.path.join(tmp, "fm_thumbs.png")
        while time.time() < deadline:
            qmp.stable_pixels(png)
            red = ink_count(png, cell_rect(1), doom_red, tol=12)
            if red > 10:
                break
            time.sleep(0.5)
        res.check("a .qoi cell shows the image's own pixels (lazily decoded)",
                  red > 10, f"doom-red in game.qoi cell: {red}")
        res.check("control: the .txt cell has none of that ink",
                  ink_count(png, cell_rect(3), doom_red, tol=12) == 0, "")

        # The JPEG: a photo has CHROMA, the generic grey/white glyph has
        # none -- count pixels whose channels spread more than 30.
        from PIL import Image
        im = Image.open(png).convert("RGB")

        def chroma(rect):
            x, y, w, h = rect
            n = 0
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    r, g, b = im.getpixel((xx, yy))
                    if max(r, g, b) - min(r, g, b) > 30:
                        n += 1
            return n

        res.check("a .jpg cell shows the photo, and the .txt cell stays grey",
                  chroma(cell_rect(2)) > 40 and chroma(cell_rect(3)) < 10,
                  f"chroma jpg={chroma(cell_rect(2))} txt={chroma(cell_rect(3))}")

        # The wheel: down scrolls DOWN (this shipped inverted once), and
        # the universal scroll_dir setting flips it for every app at the
        # kernel's single consuming point. The observable is the grid's
        # first cell riding up (cellgrid y falls) as the view scrolls.
        # ENOUGH TO OVERFLOW THE PANE, with room to spare. Fourteen was
        # exactly the old pane's capacity, so the day the bottom key row
        # was removed -- giving the panes that row of height back -- the
        # grid fitted, nothing scrolled, and a working wheel read as
        # dead. The precondition is asserted below rather than assumed.
        for i in range(22):
            dbg.send(f"sh touch {SRC}/pad{i:02}.txt")
        wait_listing(dbg, SRC, lambda names: "pad21" in [n.split(".")[0] for n in names])
        lay = wait_layout(dbg, win, lambda l: l.rows.get(0, 0) >= 25) or lay
        res.check("the icons grid has more rows than the pane can show",
                  lay.rows.get(0, 0) >= 25 and 0 in lay.cellgrid,
                  f"rows={lay.rows.get(0)} grid={lay.cellgrid.get(0)}")
        px0, py0, pw0, ph0 = lay.pane[0]
        dbg.warp_cursor(qmp, ox + px0 + pw0 // 2, oy + py0 + ph0 // 2)
        y_before = lay.cellgrid[0][1]
        qmp.wheel("down", 2)
        lay = wait_layout(dbg, win, lambda l: 0 in l.cellgrid and
                           l.cellgrid[0][1] != y_before) or lay
        res.check("wheel down scrolls the grid down",
                  0 in lay.cellgrid and lay.cellgrid[0][1] < y_before,
                  f"cell y {y_before} -> {lay.cellgrid.get(0)}")

        dbg.send("sh config set scroll_dir inverted")
        y_now = lay.cellgrid[0][1]
        qmp.wheel("down", 2)
        lay = wait_layout(dbg, win, lambda l: 0 in l.cellgrid and
                           l.cellgrid[0][1] != y_now) or lay
        res.check("scroll_dir=inverted flips it, from the one kernel knob",
                  0 in lay.cellgrid and lay.cellgrid[0][1] > y_now,
                  f"cell y {y_now} -> {lay.cellgrid.get(0)}")
        dbg.send("sh config set scroll_dir normal")

    # --- 16. associations: /bin/open and the override file --------------
    # The File Manager resolves through lib/uopen now, so an override
    # set at a PROMPT changes what a double click here opens.
    def close_others():
        for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
            if w2["title"] != TITLE:
                dbg.send(f"gui close {w2['z']}")
                time.sleep(0.3)

    def open_selected_and_wait(fragment, timeout=15.0):
        dbg.key(K_ENTER)
        deadline = time.time() + timeout
        titles = []
        while time.time() < deadline:
            titles = [w2["title"] for w2 in dbg.windows()]
            if any(fragment in t for t in titles):
                return titles
            time.sleep(0.3)
        return titles

    def spawn_out(cmd):
        """A spawned child's output can drain AFTER the prompt returns
        -- the serial read races the child's stdout. Read the command's
        response AND a cheap follow-up, and search both."""
        first = dbg.send(cmd) or ""
        time.sleep(0.4)
        second = dbg.send("sh pwd") or ""
        return first + "\n" + second

    spawn_out("sh spawn /bin/open -s .txt imgview")
    dbg.key("0x72")  # 'r' seeks readme.txt
    lay = wait_layout(dbg, win, lambda l: l.selected == "readme.txt")
    titles = open_selected_and_wait("Image")
    res.check("an override in /etc/mimeapps.conf outranks the Handles= declaration",
              any("Image" in t for t in titles), f"windows {titles}")
    close_others()

    # Retried: a reap or exit line landing on the serial console can
    # split the child's line mid-characters; a quiet retry arrives whole.
    out = ""
    for _ in range(3):
        out = spawn_out("sh spawn /bin/open -l")
        if ".txt=imgview" in out:
            break
        time.sleep(0.5)
    res.check("open -l lists the override", ".txt=imgview" in out, repr(out[:160]))

    spawn_out("sh spawn /bin/open -s .txt -")
    titles = open_selected_and_wait("readme.txt")
    res.check("clearing it (-s .txt -) hands the type back to the declaration",
              any("readme.txt" in t and "Image" not in t for t in titles),
              f"windows {titles}")
    close_others()

    out = spawn_out(f"sh spawn /bin/open {SRC}/game.qoi.nope")
    res.check("open refuses a type nothing claims, by name",
              "nothing opens" in out, repr(out[:160]))
    dbg.send("sh rm /etc/mimeapps.conf")

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
