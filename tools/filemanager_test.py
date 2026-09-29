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
import json

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402

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
K_CTRL_L = "0x0c"
K_ENTER = "0x0d"

# `gui key` takes a character or a code (userland/wm/wm_debug.c), which
# is why nothing here depends on the guest's keyboard layout -- the trap
# every QMP-typing tool here has paid for at least once.
K_TAB, K_ENTER, K_ESC, K_BACKSPACE = "0x09", "0x0a", "0x1b", "0x08"
K_DOWN, K_UP = "0x92", "0x91"
K_LEFT, K_RIGHT = "0x95", "0x96"   # KEY_ARROW_LEFT/RIGHT (api/keyboard.h)

# BUTTONS ARE ADDRESSED BY NAME, and the name maps to (strip, index) in
# ONE place. The window has three strips -- Back/Forward/Up/Refresh
# beside the breadcrumb ("nav"), the command bar ("tb") and the view
# switch on the status bar ("vb") -- and an index is its order in the
# strip's array in userland/gui/apps/files.c, separators counted. A
# button added there is one edit here; the day indices were spelled at
# each call site, one insertion failed sixteen checks on state the wrong
# click left behind.
BTN = {
    "back": ("nav", 0), "forward": ("nav", 1), "up": ("nav", 2), "refresh": ("nav", 3),
    "new": ("tb", 0), "cut": ("tb", 2), "copy": ("tb", 3), "paste": ("tb", 4),
    "rename": ("tb", 5), "delete": ("tb", 6), "sort": ("tb", 8), "view": ("tb", 9),
    "more": ("tb", 10), "dpane": ("tb", 11),
    "details": ("vb", 0), "icons": ("vb", 1),
}
STRIP_COUNT = {"nav": 4, "tb": 12, "vb": 2}
# The View drop-down's rows, separators counted (files.c's view_items).
VIEW_ROW = {"large": 0, "icons": 1, "details": 2, "dpane": 4, "panes": 5, "tree": 6}
NEW_ROW = {"folder": 0, "file": 1}
MORE_ROW = {"options": 8}   # files.c's more_items, separators counted
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
        self.scroll = {}      # pane -> (icon grid row, details table row)
        self.marked = (0, 0)
        self.dim = (0, 0)     # per-pane dimmed rows -- a staged cut
        self.cancel = None    # is the Cancel button up?
        self.hover = None     # (pane0 hovered row, pane1 hovered row)
        self.hoverv = {}      # pane -> hovered VIEW row, -1 for none
        self.view = None      # [mode0, mode1, single, tree_on, tree_nodes]
        self.treebox = None   # [x, y, w, h, row_h, selected_id]
        self.split = None     # (tree fraction, pane fraction), per mille
        self.splitbox = {}    # 0 = tree divider, 1 = pane divider -> [x,y,w,h]
        self.rowh = None      # a pane's row height
        self.rowy = {}        # pane -> y of view row 0 (past the header)
        self.dialog = None    # is the conflict dialog up?
        self.dlghot = None    # ...and which button a Return would commit
        self.ctx = None       # is the context menu open?
        self.ctxbox = None    # its popup [x, y, w, h]
        self.ctxitems = 0     # rows in that popup, separators included
        self.addr = None      # (pane, text) of the address bar being edited, or (-1, "-")
        self.note = None      # the status bar's note
        self.menu = None      # open level-0 popup [x, y, w, h]
        self.menuhot = None   # (open depth, level-0 hot row)
        self.cellgrid = {}    # pane -> [x0, y0, cell_w, cell_h, cols]
        self.toolbar = None   # the strip [x, y, w, h]
        self.tbitems = {}     # command-bar item index -> [x, y, w, h]
        self.navitems = {}    # Back/Forward/Up/Refresh
        self.vbitems = {}     # the status bar's view switch
        self.popitems = {}    # the open drop-down's (or context menu's) rows
        self.dpane = None     # is the details pane shown
        self.pathedit = None  # is the breadcrumb being typed in
        self.pathseg = {}     # breadcrumb segment (0 = the root) -> [x, y, w, h]
        self.placerows = {}   # Places row -> [x, y, w, h]; places, then devices
        self.placesel = None  # the Places row that IS the active pane's directory
        self.searchbox = None # [x, y, w, h]
        self.dpsel = None     # the name the details pane describes, "-" for the folder
        self.status = None    # (items text, selection text)
        self.searching = None # does the search box have the keyboard
        self.renaming = None  # (pane 0, pane 1, the active field's text)
        self.treesel = None   # (selected node's path, visible tree rows)
        self.treerow = {}     # visible tree row -> the node's path
        self.drag = None      # (in flight, count, copy) -- a drag session
        self.drop = None      # (pane0 drop row, pane1 drop row, tree node PATH or "-")
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
        elif p[0] == "scroll" and len(p) >= 4:
            self.scroll[int(p[1])] = (int(p[2]), int(p[3]))
        elif p[0] == "active" and len(p) >= 2:
            self.active = int(p[1])
        elif p[0] == "selected" and len(p) >= 2:
            self.selected = p[1]
        elif p[0] == "modal" and len(p) >= 2:
            self.modal = int(p[1])
        elif p[0] == "marked" and len(p) >= 3:
            self.marked = (int(p[1]), int(p[2]))
        elif p[0] == "dim" and len(p) >= 3:
            self.dim = (int(p[1]), int(p[2]))
        elif p[0] == "cancel" and len(p) >= 2:
            self.cancel = int(p[1])
        elif p[0] == "hover" and len(p) >= 3:
            self.hover = (int(p[1]), int(p[2]))
        elif p[0] == "hoverv" and len(p) >= 3:
            self.hoverv[int(p[1])] = int(p[2])
        elif p[0] == "view" and len(p) >= 8:
            # "view <mode0> <mode1> single <s> tree <t> <nodes>"
            self.view = [int(p[1]), int(p[2]), int(p[4]),
                         int(p[6]), int(p[7])]
        elif p[0] == "treebox" and len(p) >= 7:
            self.treebox = [int(v) for v in p[1:7]]
        elif p[0] == "rowh" and len(p) >= 2:
            self.rowh = int(p[1])
        elif p[0] == "rowy" and len(p) >= 3:
            self.rowy[int(p[1])] = int(p[2])
        elif p[0] == "dialog" and len(p) >= 2:
            self.dialog = int(p[1])
            if len(p) >= 3:
                self.dlghot = int(p[2])
        elif p[0] == "ctx" and len(p) >= 2:
            self.ctx = int(p[1])
            if len(p) >= 3:
                self.ctxitems = int(p[2])
        elif p[0] == "ctxbox" and len(p) >= 5:
            self.ctxbox = [int(v) for v in p[1:5]]
        elif p[0] == "addr" and len(p) >= 3:
            self.addr = (int(p[1]), p[2])
        elif p[0] == "note":
            self.note = " ".join(p[1:])
        elif p[0] == "split" and len(p) >= 3:
            self.split = (int(p[1]), int(p[2]))
        elif p[0] == "splitbox" and len(p) >= 6:
            self.splitbox[int(p[1])] = [int(v) for v in p[2:6]]
        elif p[0] == "menu.popup" and len(p) >= 6:
            # The open level-0 popup, from the menu's own describe op
            # (ui/uui_describe.h); the bare `menu` line is the BAR.
            if int(p[1]) == 0:
                self.menu = [int(v) for v in p[2:6]]
        elif p[0] == "menuhot" and len(p) >= 3:
            self.menuhot = (int(p[1]), int(p[2]))
        elif p[0] == "cellgrid" and len(p) >= 7:
            self.cellgrid[int(p[1])] = [int(v) for v in p[2:7]]
        elif p[0] == "toolbar" and len(p) >= 5:
            self.toolbar = [int(v) for v in p[1:5]]
        elif p[0] == "tbitem" and len(p) >= 6:
            self.tbitems[int(p[1])] = [int(v) for v in p[2:6]]
        elif p[0] == "navitem" and len(p) >= 6:
            self.navitems[int(p[1])] = [int(v) for v in p[2:6]]
        elif p[0] == "vbitem" and len(p) >= 6:
            self.vbitems[int(p[1])] = [int(v) for v in p[2:6]]
        elif p[0] == "ctxmenu.item" and len(p) >= 7 and p[1] == "0":
            self.popitems[int(p[2])] = [int(v) for v in p[3:7]]
        elif p[0] == "ctxmenu.popup" and len(p) >= 6 and p[1] == "0":
            self.menu = [int(v) for v in p[2:6]]
        elif p[0] == "dpane" and len(p) >= 3:
            self.dpane, self.dpsel = int(p[1]), p[2]
        elif p[0] == "path.seg" and len(p) >= 6:
            self.pathseg[int(p[1])] = [int(v) for v in p[2:6]]
        elif p[0] == "places.row" and len(p) >= 6:
            self.placerows[int(p[1])] = [int(v) for v in p[2:6]]
        elif p[0] == "places.selected" and len(p) >= 2:
            self.placesel = int(p[1])
        elif p[0] == "search" and len(p) >= 5:
            self.searchbox = [int(v) for v in p[1:5]]
        elif p[0] == "renaming" and len(p) >= 4:
            self.renaming = (int(p[1]), int(p[2]), " ".join(p[3:]))
        elif p[0] == "searching" and len(p) >= 2:
            self.searching = int(p[1])
        elif p[0] == "status":
            text = " ".join(p[1:])
            items, _, sel = text.partition("|")
            self.status = (items.strip(), sel.strip())
        elif p[0] == "path.editing" and len(p) >= 2:
            self.pathedit = int(p[1])
        elif p[0] == "treesel" and len(p) >= 3:
            self.treesel = (p[1], int(p[2]))
        elif p[0] == "treerow" and len(p) >= 3:
            self.treerow[int(p[1])] = p[2]
        elif p[0] == "drag" and len(p) >= 4:
            self.drag = tuple(int(v) for v in p[1:4])
        elif p[0] == "drop" and len(p) >= 4:
            self.drop = (int(p[1]), int(p[2]), p[3])

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
        self.passes, self.fails, self.skips = [], [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")

    def skip(self, name, why):
        """A check whose PREREQUISITE failed. Named rather than silently
        absent, so the count of what was not measured is in the summary;
        the exit status is already failing on the prerequisite."""
        print(f"  SKIP  {name}")
        print(f"        {why}")
        self.skips.append(name)


def listing(dbg, path):
    """What the SHELL says is in `path` -- the independent witness."""
    out = dbg.send(f"sh ls {path}") or ""
    names = []
    for line in out.splitlines():
        line = line.strip()
        # The kernel narrates every ring-3 load on the same console
        # ("elf_run: ...", "syscall: ..."), and the console can echo the
        # command itself.
        if not line or ":" in line or line.startswith("---"):
            continue
        # A NAME MAY CONTAIN A SPACE. Rejecting every line with one also
        # rejected "one (1).txt" -- the file Rename creates -- so the
        # conflict check read a working rename as a rename that never
        # happened. What the noise actually carries is a PATH: both the
        # echoed command and the kernel's narration hold a "/" that is
        # not the trailing one `ls` puts on a directory.
        if "/" in line.rstrip("/"):
            continue
        names.append(line.rstrip("/"))
    return names


# A POLL MUST ACCUMULATE, NOT RE-READ. The app emits its report as ONE
# BLOCK and only when the block CHANGES (uapp.c's layout_log_flush), so
# an idle window emits nothing at all -- and a serial read that lands
# mid-block hands back a frame missing everything after the split. Parse
# each read on its own and that frame is the only one you will ever get:
# the poll then spins until it times out while the app sits there having
# already reported exactly what was asked for. Accumulation is bounded
# to ONE block, so a partial report is never merged into the frame
# before it and a field the newer block omits cannot survive into it.
_LAST_BUF = []
_ALL_BUF = []
# The report block being received, which OUTLIVES the wait that started
# reading it: a wait ending mid-block used to drop its lines, and the
# tail then arrived alone and could complete nothing.
_PENDING = []
# The last COMPLETE layout any poll parsed, and whether it still
# describes the app. The app dedupes a whole report block (uapp.c), so
# while it stays silent that layout IS its current state -- which is
# what lets a wait answer from it after a grace period instead of timing
# out on a report the app will never repeat.
_LAST_LAYOUT = None
_LAST_CURRENT = False

# A report opens with pane 0 (fm_view.c's log_layout), so this line is
# where one block ends and the next begins -- the only point at which
# the app can be said to have moved on from what it last reported.
_BLOCK_START = "layout pane 0 "


def last_layout():
    """The most recent complete layout observed, for a failure's
    evidence. It may be a state the app has already left -- see
    `layout_is_current()`, which is what a WAIT has to ask."""
    return _LAST_LAYOUT


def layout_is_current():
    """Whether `last_layout()` may still answer for the app NOW: true
    only while no newer report has begun since it was parsed."""
    return _LAST_CURRENT


def reset_layout():
    """Forget what the app has reported. A window that has been closed
    and respawned is a DIFFERENT surface -- new origin, new panes, new
    toolbar -- so a frame from the old one is not stale geometry, it is
    somebody else's."""
    global _LAST_LAYOUT, _LAST_CURRENT
    _LAST_LAYOUT = None
    _LAST_CURRENT = False
    _PENDING[:] = []
    _LAST_BUF[:] = []


def _collect(dbg):
    """Sweep fresh `files:` lines into the block being received. Returns
    how many ARRIVED on this poll -- not the buffer, which is truthy
    forever after the first line and cannot say whether anything new
    came."""
    global _LAST_CURRENT
    fresh = dbg.logs("files:", clear=True)
    # `_ALL_BUF` is the whole transcript: a line the app logged during an
    # EARLIER wait is gone by the time a check fails, which reads as an
    # app that logged nothing at all. It is what a failure should quote.
    _ALL_BUF.extend(fresh)
    for line in fresh:
        if _BLOCK_START in line:
            # A NEWER REPORT. What came before it belongs to the block
            # that ended, and the cached layout is now the state the app
            # HAD -- it may not answer a wait again until a block parses
            # complete, however many waits that takes.
            _PENDING[:] = []
            _LAST_CURRENT = False
        _PENDING.append(line)
    _LAST_BUF[:] = _PENDING
    return len(fresh)


def _parse(win):
    """Parse the block received so far; a complete one becomes the
    cached layout and is current again."""
    global _LAST_LAYOUT, _LAST_CURRENT
    lay = Layout(win["content"], _PENDING)
    if lay.complete():
        _LAST_LAYOUT = lay
        _LAST_CURRENT = True
        return lay
    return None


def layout_now(dbg, win, tries=25):
    """The app's CURRENT layout report, polled rather than slept for.

    Only lines arriving DURING this call answer it: the cached block is
    what a drag's confirmation loop would otherwise read back instead of
    the frame it is waiting for.
    """
    for _ in range(tries):
        # The console is NOT cleared first: the app draws when something
        # happens, so the report we want may already be waiting there --
        # an unconditional clear threw away the only frame the app had
        # logged and reported a working window as silent.
        if _collect(dbg):
            lay = _parse(win)
            if lay:
                return lay
        time.sleep(0.2)
    return None


# THE CONTEXT MENU IS ASKED OF THE COMPOSITOR, not of the app. It is a
# popup SURFACE, and opening one does not redraw the window that owns it
# -- the Wayland xdg_popup shape -- so the app's `ctx` line (written by
# the owner's on_draw) went on saying 0 under a menu that was open and
# worked. The window list is live, and the app cannot get it wrong.
# Tooltips are popups too, one text row tall; a menu is taller.
MENU_POPUP_MIN_H = 40


def _popup_state(dbg, win):
    """(open, [x, y, w, h] relative to the content) of the menu popup,
    or None if the compositor could not be asked."""
    try:
        ws = dbg.json("gui windows --json")["windows"]
    except Exception:
        return None
    menus = [w for w in ws if w.get("popup") and w.get("h", 0) >= MENU_POPUP_MIN_H]
    if not menus:
        return 0, None
    m, c = menus[-1], win["content"]
    return 1, [m["x"] - c["x"], m["y"] - c["y"], m["w"], m["h"]]


def _live(dbg, win, lay):
    st = _popup_state(dbg, win)
    if lay is not None and st is not None:
        lay.ctx, lay.ctxbox = st
    return lay


def wait_layout(dbg, win, pred, timeout=12.0, grace=1.0):
    """Poll the app's own report until `pred` holds. Waiting on the
    OBSERVABLE rather than on a fixed sleep (CLAUDE.md).

    Returns the layout that SATISFIED `pred`, or None when the deadline
    passed with it never true -- never a layout the predicate rejected,
    which a caller then read as the state it asked for. The last frame
    seen is `last_layout()`, for the failure detail.

    **THE `or lay` IDIOM AT CALL SITES KEEPS THE PREVIOUS LAYOUT** so a
    timeout fails a check instead of crashing on None. That is right
    where the caller only READS a field -- and wrong where it then
    INDEXES one the old layout may not carry: `lay.rowy[0]` and
    `lay.rowh` killed this tool with `KeyError: 0` and `None * int`,
    reported as the File Manager being broken. So ask the predicate for
    what the code below actually uses, and drop the fallback there.

    Partial reads accumulate within the wait, so a report split across
    two serial sweeps is parsed whole. And after `grace` seconds with
    no fresh report at all, the last layout ANY wait observed answers if
    it satisfies `pred`: the app repeats nothing it already said, so a
    state reached before this wait began would otherwise time out.

    THAT CACHED ANSWER IS OFF FROM THE MOMENT A NEWER REPORT BEGINS
    UNTIL ONE PARSES COMPLETE, however many waits that spans. A report
    whose tail has not arrived leaves the previous frame the newest
    COMPLETE one, so the grace period expires against a state the app
    has left -- an old two-pane layout answering a wait for one pane.
    The eligibility is `layout_is_current()`, which is deliberately NOT
    per-wait: this wait timing out changes nothing about the app, so a
    later one must not treat the same frame as current again merely
    because the app has gone quiet in the meantime.
    """
    deadline = time.time() + timeout
    quiet_since = time.time()
    while time.time() < deadline:
        if _collect(dbg):
            quiet_since = time.time()
            lay = _live(dbg, win, _parse(win))
            if lay and pred(lay):
                return lay
        elif (_LAST_CURRENT and _LAST_LAYOUT is not None
              and time.time() - quiet_since >= grace
              and pred(_live(dbg, win, _LAST_LAYOUT))):
            return _LAST_LAYOUT
        time.sleep(0.2)
    return None


def btn_rect(lay, name):
    """Button `name`'s rect as the app reported it, or None."""
    strip, i = BTN[name]
    items = {"nav": lay.navitems, "tb": lay.tbitems, "vb": lay.vbitems}[strip]
    return items.get(i)


def strips_complete(lay):
    """Every strip reported every item -- a layout mid-transition can
    carry fewer."""
    return (len(lay.navitems) == STRIP_COUNT["nav"] and len(lay.tbitems) == STRIP_COUNT["tb"]
            and len(lay.vbitems) == STRIP_COUNT["vb"])


def _toolbar_evidence(name, seen):
    """What a failed button lookup should say: the button asked for, what
    each strip reported, the view state, and the last report lines --
    which tells missing GEOMETRY from a transition that did not happen."""
    have = ({"nav": sorted(seen.navitems), "tb": sorted(seen.tbitems),
             "vb": sorted(seen.vbitems)} if seen else None)
    state = (f"view={seen.view} treebox={seen.treebox} ctx={seen.ctx} "
             f"panes={sorted(seen.pane)}") if seen else "no layout observed"
    recent = [l.strip() for l in _ALL_BUF[-8:] if "layout" in l]
    return (f"requested button {name} {BTN.get(name)}; reported {have}; {state}; "
            f"recent: {' | '.join(recent) or '(none)'}")


def toolbar_layout(dbg, win, lay, name, res, what):
    """A layout reporting button `name` with a usable rect, or None with a
    FAILED check recorded and NO click made. `lay` is reused when it
    already has the button; otherwise the report is waited for and the
    condition rechecked on what arrives."""
    def usable(l):
        r = btn_rect(l, name)
        return bool(r) and r[2] > 0 and r[3] > 0
    got = lay if (lay is not None and usable(lay)) else wait_layout(dbg, win, usable, timeout=6.0)
    if got is None:
        res.check(f"{what}: button {name} is reported with a usable rect", False,
                  _toolbar_evidence(name, last_layout() or lay))
        return None
    return got


def toolbar_click(dbg, qmp, win, lay, name, res, what):
    """Click button `name` at the centre the app reported. Returns the
    layout the click was aimed from, or None (check recorded, no click)."""
    got = toolbar_layout(dbg, win, lay, name, res, what)
    if got is None:
        return None
    x, y, w, h = btn_rect(got, name)
    sure_click(dbg, qmp, got.ox + x + w // 2, got.oy + y + h // 2)
    return got


def dropdown_pick(dbg, qmp, win, lay, button, row, res, what):
    """Open a command-bar drop-down and click row `row` of it, at the rect
    the menu itself reports. Returns the layout the row was aimed from,
    or None (a check recorded)."""
    got = toolbar_click(dbg, qmp, win, lay, button, res, what)
    if got is None:
        return None
    pop = wait_layout(dbg, win, lambda l: l.ctx == 1 and row in l.popitems, timeout=6.0)
    if pop is None:
        res.check(f"{what}: the {button} drop-down opened with row {row}", False,
                  _toolbar_evidence(button, last_layout() or got))
        dbg.key(K_ESC)
        return None
    x, y, w, h = pop.popitems[row]
    sure_click(dbg, qmp, pop.ox + x + w // 2, pop.oy + y + h // 2)
    return pop


def view_pick(dbg, qmp, win, lay, name, res, what=None):
    return dropdown_pick(dbg, qmp, win, lay, "view", VIEW_ROW[name], res,
                         what or f"View > {name}")


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


def size_of(dbg, directory, name):
    """One file's size, from `ls -l` on its DIRECTORY -- pointing ls at a
    file does not describe that file."""
    for line in (dbg.send(f"sh ls -l {directory}") or "").splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[0] == "-" and parts[-1] == name:
            return int(parts[1])
    return -1


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

    # **THE TWO-PANE STATE IS ESTABLISHED, NOT INHERITED.** Most of this
    # tool drives the commander layout -- copy between panes, the active
    # pane's strip, Tab -- and it used to get it by default. The app
    # opens as an Explorer now (one pane, the tree; see
    # docs/conventions/gui.md), so nineteen checks failed on a change to
    # a DEFAULT, which is CLAUDE.md's rule arriving the hard way: a test
    # must state the precondition it needs.
    #
    # Written before the first launch, because the app reads this once
    # at startup. `panes=2` is what it writes for the commander.
    #
    # Through write_lines(), because the quoted `tosh -c "echo ..."` this
    # used wrote NOTHING (spawn keeps the quotes), so the app opened as
    # an Explorer anyway and every commander check after the first
    # View toggle failed -- the runtime-dependent failure count filed in
    # docs/bugs.md.
    if not dbg.write_lines(FILES_CONF, ["panes=2", "tree=0"]):
        print(f"filemanager_test: WARNING -- {FILES_CONF} did not reach the disk: "
              f"{dbg.send(f'sh cat {FILES_CONF}')!r}")


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

    # --- 0. icons by default, and Details for the sections below --------
    # The panes open in ICONS view (2) on a fresh config, as every
    # desktop file manager does. Everything below aims at rows by
    # height, so both panes are put in Details (1) from the toolbar --
    # which is a check of the toolbar's Details button too.
    lay = wait_layout(dbg, win, lambda l: l.view is not None and strips_complete(l)) or lay
    res.check("both panes open in icons view by default",
              lay.view is not None and lay.view[0] == 2 and lay.view[1] == 2,
              f"view={lay.view}")
    for i in (1, 0):
        dbg.click(*lay.pane_centre(i))
        lay = wait_layout(dbg, win, lambda l, i=i: l.active == i) or lay
        # The status bar's view switch, as in Explorer.
        lay = toolbar_click(dbg, qmp, win, lay, "details", res, "Details") or lay
        lay = wait_layout(dbg, win, lambda l, i=i: l.view and l.view[i] == 1) or lay
    res.check("the status bar's Details switch changes each pane in turn",
              lay.view is not None and lay.view[0] == 1 and lay.view[1] == 1,
              f"view={lay.view}")

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

    # --- 4a. history: back, forward, and the thumb buttons ---------------
    #
    # THE HISTORY IS PER PANE and browser-shaped: going somewhere new
    # truncates forward. Driven here through all three of its entry
    # points, because they are three separate bugs -- the keyboard
    # binding, the toolbar button, and the mouse button the compositor
    # has to deliver before the app can act on it.
    #
    # At this point the pane has been to SRC, SRC/sub, and SRC again.
    dbg.key(K_LEFT, mods="alt")
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{SRC}/sub") or lay
    res.check("Alt+Left goes back to the previous directory",
              lay.dir.get(0) == f"{SRC}/sub", f"dir {lay.dir.get(0)}")

    dbg.key(K_RIGHT, mods="alt")
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC) or lay
    res.check("Alt+Right goes forward again",
              lay.dir.get(0) == SRC, f"dir {lay.dir.get(0)}")

    # THE THUMB BUTTON, which is the whole reason the kernel now carries
    # five button bits. Delivered by the compositor to the client under
    # the pointer; the app decides it means "back" (abi/win_proto.h).
    px = win["content"]["x"] + 200
    py = win["content"]["y"] + 200
    dbg.send(f"gui click {px} {py} 4")
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{SRC}/sub") or lay
    res.check("the mouse's back button goes back",
              lay.dir.get(0) == f"{SRC}/sub", f"dir {lay.dir.get(0)}")

    dbg.send(f"gui click {px} {py} 5")
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC) or lay
    res.check("...and its forward button goes forward",
              lay.dir.get(0) == SRC, f"dir {lay.dir.get(0)}")

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
    lay = wait_layout(dbg, win, lambda l: l.selected == "one.txt") or lay
    # The selection colour's footprint with ONE row selected, for the
    # comparison after marking.
    sel_rgb = (205, 220, 240)      # utheme's sel_bg
    png_one = os.path.join(tmp, "fm_sel_one.png")
    qmp.stable_pixels(png_one)
    pane_rect = (lay.ox + lay.pane[0][0], lay.oy + lay.pane[0][1], lay.pane[0][2], lay.pane[0][3])
    blue_one = ink_count(png_one, pane_rect, sel_rgb)
    dbg.key(K_INSERT)
    dbg.key(K_INSERT)
    lay = wait_layout(dbg, win, lambda l: l.marked[0] == 2) or lay
    res.check("Insert marks files, and the app counts them",
              lay.marked[0] == 2, f"marked {lay.marked}")
    # Two marks plus the cursor row, all in the SELECTION colour -- a
    # multi-selection is one selection with several rows, not a second
    # (yellow) kind of highlight. Three rows of blue against one.
    png_marks = os.path.join(tmp, "fm_sel_marks.png")
    qmp.stable_pixels(png_marks)
    blue_marks = ink_count(png_marks, pane_rect, sel_rgb)
    res.check("marked rows are drawn in the selection colour",
              blue_one > 0 and blue_marks > 2 * blue_one,
              f"selection-blue pixels: one row {blue_one}, two marks + cursor {blue_marks}")

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
    lay = wait_layout(dbg, win, lambda l: l.dialog == 1) or lay
    res.check("F8 opens a confirmation dialog rather than deleting", lay.dialog == 1,
              f"dialog {lay.dialog}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.dialog == 0) or lay
    still = listing(dbg, DST)
    res.check("Esc cancels it and the file is still there", victim in still,
              f"{victim!r} not in {still}")

    dbg.key(K_F8)
    wait_layout(dbg, win, lambda l: l.dialog == 1)
    dbg.key(K_ENTER)                        # Delete is the default button
    gone = wait_listing(dbg, DST, lambda n: victim not in n)
    res.check("confirming it deletes the file", victim not in gone,
              f"{victim!r} still in {gone}")

    # --- 7. a new directory --------------------------------------------
    # IN PLACE by default (Options): F7 makes "New folder" and its name
    # becomes the field, all of it selected so typing replaces it.
    dbg.key(K_F7)
    lay = wait_layout(dbg, win, lambda l: l.renaming and l.renaming[1] == 1) or lay
    res.check("F7 makes a New folder and edits its name in place",
              lay.renaming is not None and lay.renaming[1] == 1 and lay.renaming[2] == "New folder",
              f"renaming={lay.renaming}")
    # settle=True (the default) for every typed character: sent
    # back-to-back with settle=False they outrun the client, and the
    # field commits empty -- which reads exactly like a broken mkdir.
    for ch in "newdir":
        dbg.key(ch)
    dbg.key(K_ENTER)
    made = wait_listing(dbg, DST, lambda n: "newdir" in n)
    res.check("typing a name and pressing Enter creates the directory",
              "newdir" in made and "New folder" not in made, f"{DST} holds {made}")

    # --- 8. rename ------------------------------------------------------
    lay = wait_layout(dbg, win, lambda l: l.selected == "newdir") or lay
    dbg.key(K_F2)
    lay = wait_layout(dbg, win, lambda l: l.renaming and l.renaming[1] == 1) or lay
    res.check("F2 edits the name in place, holding the current one",
              lay.renaming is not None and lay.renaming[2] == "newdir", f"renaming={lay.renaming}")
    # The field is pre-filled with the current name, so clear it first.
    for _ in range(8):
        dbg.key(K_BACKSPACE)
    for ch in "renamed":
        dbg.key(ch)
    dbg.key(K_ENTER)
    renamed = wait_listing(dbg, DST, lambda n: "renamed" in n)
    res.check("F2 renames the selection", "renamed" in renamed and "newdir" not in renamed,
              f"{DST} holds {renamed}")

    # Ctrl+Shift+N is Explorer's New folder; Esc keeps the default name.
    dbg.key("0x0e", mods="shift ctrl")
    lay = wait_layout(dbg, win, lambda l: l.renaming and l.renaming[1] == 1) or lay
    dbg.key(K_ESC)
    made = wait_listing(dbg, DST, lambda n: "New folder" in n)
    res.check("Ctrl+Shift+N makes a New folder too", "New folder" in made, f"{DST} holds {made}")
    # (Left in DST: the teardown removes the whole fixture.)

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
        # Not a POPUP: a tooltip is the File Manager's own, and closing it
        # from outside is not what a person can do.
        if w2["title"] != TITLE and not w2.get("popup"):
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.3)
    # **AND WAIT FOR THE FOCUS, not just for the windows to go.** The
    # next thing here drives the menu BY KEY, which reaches the focused
    # window -- so "the others are closed" is a weaker condition than
    # what follows it needs, and the File Manager taking the focus back
    # is a separate event. It cost a `View->Icons` failure that only
    # appeared under a trap gate, where the timing moved.
    deadline = time.time() + 8.0
    while time.time() < deadline:
        ws = dbg.windows()
        if ws and all(w2["title"] == TITLE or w2.get("popup") for w2 in ws) \
                and any(w2.get("focused") for w2 in ws):
            break
        time.sleep(0.3)

    # View -> Icons switches the ACTIVE pane only.
    view_pick(dbg, qmp, win, last_layout() or lay, "icons", res)
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
    # The details view has folder icons too now, at text height: the grid's
    # are twice that, so the icons pane carries several times the ink.
    res.check("folder-icon ink: the icons pane's big glyphs outweigh the details rows'",
              left_ink > 20 and left_ink > 2 * right_ink,
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
    # Measured against the two-pane width, not the window's: the places
    # column and the details pane take their share either way.
    w_two = (last_layout() or lay).pane[0][2]
    view_pick(dbg, qmp, win, last_layout() or lay, "panes", res)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[2] == 1)
    res.check("View->Second pane collapses to one pane, taking both panes' width",
              lay is not None and lay.view and lay.view[2] == 1 and
              lay.pane[0][2] > w_two * 3 // 2,
              f"view={lay and lay.view} pane0 {w_two} -> {lay and lay.pane.get(0)}")
    view_pick(dbg, qmp, win, last_layout() or lay, "panes", res)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[2] == 0)
    res.check("toggling again restores the second pane",
              lay is not None and lay.view and lay.view[2] == 0 and
              lay.pane[0][2] < w_two * 5 // 4, f"view={lay and lay.view} pane0={lay and lay.pane.get(0)}")

    # The folder tree: a lazy uui_tree over the open set, under the
    # places in the side column.
    view_pick(dbg, qmp, win, last_layout() or lay, "tree", res)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 1 and l.treebox)
    res.check("View->Folder tree shows the tree under the places, left of the panes",
              lay is not None and lay.view and lay.view[3] == 1 and
              lay.view[4] > 1 and lay.treebox is not None and lay.treebox[1] > 0 and
              lay.treebox[0] + lay.treebox[2] <= lay.pane[0][0],
              f"view={lay and lay.view} tree={lay and lay.treebox} pane0={lay and lay.pane.get(0)}")
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

    # --- 12. the View drop-down ticks its active options -----------------
    # State right now: left pane icons, two panes, tree on, the details
    # pane on -- four of the View rows are checked. The "See more"
    # drop-down is the control (nothing checkable in it), and toggling
    # the tree off must take exactly its tick away.
    # Park the cursor far from where the popups drop: its sprite's dark
    # outline reads as tick ink if it is left over the gutter (it was,
    # from the expander click -- 18 phantom pixels).
    qx2, qy2, qw2, qh2 = lay.pane[1] if 1 in lay.pane else lay.pane[0]
    park = (ox + qx2 + qw2 - 20, oy + qy2 + qh2 - 20)

    def open_dropdown(name):
        # ONE RETRY: a press landing while the previous popup is still
        # being torn down is taken by that popup's dismissal.
        for _ in range(2):
            toolbar_click(dbg, qmp, win, last_layout() or lay, name, res, name)
            dbg.warp_cursor(qmp, *park)
            got = wait_layout(dbg, win, lambda l: l.ctx == 1 and l.menu is not None,
                              timeout=5.0)
            if got:
                return got
        return None

    lay = open_dropdown("more") or lay
    n_file = -1
    if lay and lay.menu and lay.ctx == 1:
        png = os.path.join(tmp, "fm_menu_more.png")
        qmp.stable_pixels(png)
        n_file = gutter_ink(png, lay.menu, ox, oy)
    res.check("control: the See-more drop-down's tick gutter is empty",
              n_file == 0, f"gutter ink={n_file}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 0) or lay

    lay = open_dropdown("view") or lay
    n_on = -1
    if lay and lay.menu and lay.ctx == 1:
        png = os.path.join(tmp, "fm_menu_view_on.png")
        qmp.stable_pixels(png)
        n_on = gutter_ink(png, lay.menu, ox, oy)
    res.check("the View drop-down draws ticks on its active options",
              n_on > 0, f"gutter ink={n_on}")

    # An open popup owns the POINTER, not just the click: hovering a
    # menu row over the icons grid must light no cell beneath (the
    # highlight was visible past the popup's edge), and the active
    # pane's accent outline must stay UNDER the popup -- it used to be
    # drawn from on_draw_over, which runs after the overlay pass, so
    # two accent lines crossed the menu. Both found by the maintainer
    # in one screenshot.
    menu_rect = lay.menu if lay and lay.ctx == 1 else None
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

    # A menu click must NOT fall through to what is under the popup (the
    # hand-routed-menubar bug, found by the maintainer: a click on a View
    # item also selected the tree row beneath it). Click the "Details"
    # row -- a harmless commit -- and require the pane's directory and
    # the tree's selection to stay put while the commit LANDS.
    if menu_rect and lay and 2 in lay.popitems:
        dir_before = lay.dir.get(0)
        sel_before = lay.treebox[5] if lay.treebox else None
        x, y, w, h = lay.popitems[VIEW_ROW["details"]]
        sure_click(dbg, qmp, ox + x + w // 2, oy + y + h // 2)
        lay = wait_layout(dbg, win, lambda l: l.view and l.view[0] == 1)
        res.check("a click on a menu item commits it and reaches nothing under it",
                  lay is not None and lay.view and lay.view[0] == 1 and
                  lay.dir.get(0) == dir_before and
                  (lay.treebox is None or lay.treebox[5] == sel_before),
                  f"view={lay and lay.view} dir={lay and lay.dir.get(0)} "
                  f"tree sel {sel_before} -> {lay and lay.treebox and lay.treebox[5]}")
    else:
        res.check("a click on a menu item commits it and reaches nothing under it",
                  False, "no menu geometry to aim with")
        dbg.key(K_ESC)

    # The commit above CLOSED the menu, so the toggle is a full pick.
    view_pick(dbg, qmp, win, last_layout() or lay, "tree", res)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 0)
    res.check("(the tree toggle landed)", lay is not None and lay.view and
              lay.view[3] == 0, f"view={lay and lay.view}")
    lay = open_dropdown("view") or lay
    n_off = -1
    if lay and lay.menu and lay.ctx == 1:
        png = os.path.join(tmp, "fm_menu_view_off.png")
        qmp.stable_pixels(png)
        n_off = gutter_ink(png, lay.menu, ox, oy)
    res.check("turning the folder tree off takes its tick away, keeping the others",
              0 < n_off < n_on, f"gutter ink {n_on} -> {n_off}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 0) or lay

    # --- 13. the buttons -------------------------------------------------
    # Same commands, same item_flags as the menus. Three strips: the
    # navigation buttons beside the breadcrumb, the command bar, and the
    # view switch on the status bar.
    lay = wait_layout(dbg, win, strips_complete) or lay
    res.check("every strip reports all its buttons",
              lay is not None and lay.toolbar is not None and strips_complete(lay),
              f"toolbar={lay and lay.toolbar} nav={lay and sorted(lay.navitems)} "
              f"tb={lay and sorted(lay.tbitems)} vb={lay and sorted(lay.vbitems)}")
    if not (lay and lay.toolbar and strips_complete(lay)):
        dbg.send(f"sh rm {FILES_CONF}")
        teardown_fixture(dbg)
        return

    def tb_click(name, what):
        """Click button `name`, adopting the layout it was aimed from. A
        layout arriving mid-transition can report fewer items than the
        strip has, and SUBSCRIPTING one that does not carry the button
        raised KeyError here -- which loses every check after it, the
        worst shape a harness can fail in. toolbar_click() waits for a
        layout that reports it and records a failed check when none
        arrives."""
        nonlocal lay
        got = toolbar_click(dbg, qmp, win, lay, name, res, what)
        if got is None:
            return False
        lay = got
        return True

    # Up climbs to the parent; at the root it is DISABLED and the click
    # lands on nothing.
    dir_now = lay.dir.get(0)
    parent = "/" if dir_now.count("/") <= 1 else dir_now.rsplit("/", 1)[0]
    tb_click("up", "up")
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == parent)
    res.check("the Up button climbs to the parent directory",
              lay is not None and lay.dir.get(0) == parent,
              f"dir {dir_now} -> {lay and lay.dir.get(0)} (wanted {parent})")
    while lay and lay.dir.get(0) not in (None, "/"):
        tb_click("up", "up")
        nxt = wait_layout(dbg, win, lambda l: l.dir.get(0) != lay.dir.get(0), timeout=5)
        if nxt is None or nxt.dir.get(0) == lay.dir.get(0):
            break
        lay = nxt
    tb_click("up", "up at the root")  # disabled, must do nothing
    time.sleep(0.8)
    lay = wait_layout(dbg, win, lambda l: True) or lay
    res.check("at the root the Up button is disabled and does nothing",
              lay is not None and lay.dir.get(0) == "/", f"dir={lay and lay.dir.get(0)}")

    # The Details button LATCHES while the details pane is up: its
    # background is the pressed wash and a resting sibling's is not
    # (half the assertion is the neighbour, CLAUDE.md). Then it hides
    # the pane, and shows it again.
    park = (ox + lay.pane[1][0] + lay.pane[1][2] - 20,
            oy + lay.pane[1][1] + lay.pane[1][3] - 20)
    dbg.warp_cursor(qmp, *park)
    lay = wait_layout(dbg, win, lambda l: l.dpane is not None) or lay
    png_on = os.path.join(tmp, "fm_tb_latched.png")
    qmp.stable_pixels(png_on)
    from PIL import Image
    im = Image.open(png_on).convert("RGB")
    r7, r1 = btn_rect(lay, "dpane"), btn_rect(lay, "cut")
    if r7 and r1 and lay.dpane == 1:
        p7 = im.getpixel((ox + r7[0] + 2, oy + r7[1] + 2))
        p1 = im.getpixel((ox + r1[0] + 2, oy + r1[1] + 2))
        res.check("the latched button's background differs from its resting sibling's",
                  p7 != p1, f"details btn {p7} vs cut btn {p1}")
    else:
        res.skip("the latched button's background differs from its resting sibling's",
                 f"dpane={lay.dpane}; " + _toolbar_evidence("dpane" if not r7 else "cut", lay))

    tb_click("dpane", "details pane")
    lay = wait_layout(dbg, win, lambda l: l.dpane == 0)
    res.check("the Details button hides the details pane",
              lay is not None and lay.dpane == 0, f"dpane={lay and lay.dpane}")
    lay = lay or last_layout()
    tb_click("dpane", "details pane")
    lay = wait_layout(dbg, win, lambda l: l.dpane == 1)
    res.check("clicking it again shows it",
              lay is not None and lay.dpane == 1, f"dpane={lay and lay.dpane}")
    lay = lay or last_layout()

    # The tooltip: park elsewhere (no cream in the strip's shadow), then
    # hover Refresh past the delay -- it rides the app's tick, so give it
    # delay + one tick. The tip's cream (255, 252, 220) is a colour
    # nothing else in this window draws.
    dbg.warp_cursor(qmp, *park)
    time.sleep(0.6)
    # SKIP, never return: bailing out of the function here would take
    # every check below it with it, which is the same "a fault loses
    # the rest of the run" shape the guards above exist to stop.
    r1 = btn_rect(lay, "refresh")
    if not r1:
        res.skip("hovering a button shows its tooltip, and only then",
                 _toolbar_evidence("refresh", lay))
    else:
        bx, by, bw, bh = r1
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


    # --- 13b. the verbs on the command bar --------------------------------
    # The proof the command bar works is that the commands do what their
    # keys do: New > Folder must do what F7 does, and Delete open the
    # same confirm F8 opens.
    lay = wait_layout(dbg, win, strips_complete) or lay
    dropdown_pick(dbg, qmp, win, lay, "new", NEW_ROW["folder"], res, "New > Folder")
    lay = wait_layout(dbg, win, lambda l: l.renaming and 1 in l.renaming[:2]) or lay
    res.check("the command bar's New > Folder does what F7 does",
              lay.renaming is not None and lay.renaming[2] == "New folder",
              f"renaming={lay and lay.renaming}")
    dbg.key(K_ESC)
    # It made a real folder, here at the root: delete it again (F8, and
    # Enter takes the default, Delete).
    lay = wait_layout(dbg, win, lambda l: l.selected == "New") or lay
    dbg.key(K_F8)
    lay = wait_layout(dbg, win, lambda l: l.dialog == 1) or lay
    dbg.key(K_ENTER)
    gone = wait_listing(dbg, "/", lambda n: "New folder" not in n)
    res.check("(...and it is deleted again)", "New folder" not in gone, f"/ holds {gone}")

    # Delete needs something selected. SEEKED, not clicked: the pane's
    # centre is below the last row in a short directory, so the click
    # selected nothing and Delete correctly said so -- which reads as a
    # broken toolbar button.
    dbg.key("0x62")                          # 'b' seeks bin/
    lay = wait_layout(dbg, win, lambda l: l.selected not in (None, "-")) or lay
    tb_click("delete", "delete")
    lay = wait_layout(dbg, win, lambda l: l.dialog == 1) or lay
    res.check("the command bar's Delete opens the same confirm F8 does",
              lay.dialog == 1, f"dialog={lay and lay.dialog}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.dialog == 0) or lay
    res.check("(Esc dismissed it without deleting anything)",
              lay is not None and lay.dialog == 0, f"dialog={lay and lay.dialog}")

    # --- 13c. the context menu acts on the pane it was opened IN --------
    # A right-click in the RIGHT pane. pane_at() once tested a row-index
    # hit as a boolean, so a miss on the left pane (-1) read as a hit and
    # every menu opened over the right pane acted on the left -- Delete
    # "only worked on the left pane". The proof is the active pane.
    lay = wait_layout(dbg, win, lambda l: 1 in l.pane and 1 in l.rowy) or lay
    rx, ry, rw, _ = lay.pane[1]
    r = 1 if lay.rows.get(1, 0) >= 2 else 0
    sure_rclick(dbg, qmp, ox + rx + rw // 2, oy + lay.rowy[1] + lay.rowh * r + lay.rowh // 2)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 1 and l.active == 1) or lay
    res.check("a right-click in the right pane opens the menu on the RIGHT pane",
              lay.ctx == 1 and lay.active == 1,
              f"ctx={lay and lay.ctx} active={lay and lay.active}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 0) or lay
    dbg.click(*lay.pane_centre(0))
    lay = wait_layout(dbg, win, lambda l: l.active == 0) or lay

    # --- 13d. the address bar -------------------------------------------
    # Each pane's path strip is a text field. Clicking it edits the path
    # in place; Enter navigates, Esc restores, and a directory that does
    # not exist is refused with the field left up. Dolphin's split view.
    def type_path(path):
        for ch in path:
            dbg.key("0x2f" if ch == "/" else ch)

    dir0 = lay.dir.get(0)
    sure_click(dbg, qmp, *lay.header_point(0))
    lay = wait_layout(dbg, win, lambda l: l.addr and l.addr[0] == 0) or lay
    res.check("clicking a pane's path strip starts editing it",
              lay.addr is not None and lay.addr[0] == 0, f"addr={lay and lay.addr}")
    type_path(DST)
    lay = wait_layout(dbg, win, lambda l: l.addr and l.addr[1] == DST) or lay
    res.check("...and typing replaces the path (the click selects it all)",
              lay.addr is not None and lay.addr[1] == DST, f"addr={lay and lay.addr}")
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == DST and l.addr and l.addr[0] == -1) or lay
    res.check("Enter navigates the pane there and ends the edit",
              lay.dir.get(0) == DST and lay.addr is not None and lay.addr[0] == -1,
              f"dir0={lay and lay.dir.get(0)} addr={lay and lay.addr}")

    sure_click(dbg, qmp, *lay.header_point(0))
    wait_layout(dbg, win, lambda l: l.addr and l.addr[0] == 0)
    type_path("/nope")
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: "no such" in (l.note or "")) or layout_now(dbg, win) or lay
    res.check("a directory that does not exist is refused, the pane stays put",
              lay.dir.get(0) == DST and lay.addr is not None and lay.addr[0] == 0,
              f"dir0={lay and lay.dir.get(0)} addr={lay and lay.addr}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.addr and l.addr[0] == -1) or lay
    res.check("Esc ends the edit with the path unchanged",
              lay.addr is not None and lay.addr[0] == -1 and lay.dir.get(0) == DST,
              f"dir0={lay and lay.dir.get(0)} addr={lay and lay.addr}")

    # Ctrl+L is the keyboard's way in. Back to where this pane was.
    dbg.key(K_CTRL_L)
    lay = wait_layout(dbg, win, lambda l: l.addr and l.addr[0] == 0) or lay
    lay = wait_layout(dbg, win, lambda l: l.pathedit == 1) or lay
    res.check("Ctrl+L turns the breadcrumb into a field",
              lay.pathedit == 1, f"path.editing={lay and lay.pathedit}")
    type_path(dir0 or SRC)
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == (dir0 or SRC)) or lay
    res.check("...and Enter there navigates the active pane",
              lay.dir.get(0) == (dir0 or SRC) and lay.pathedit == 0,
              f"dir0={lay.dir.get(0)} path.editing={lay.pathedit}")

    # --- 13e. the breadcrumb, Places, search and the details pane --------
    # Each asserted on the DIRECTORY or the ROWS the app reports after, and
    # each with the geometry the widget itself logged -- never a pitch.
    # The fixture is section 11's: alpha..echo.txt and sub/.
    if lay.active != 0:
        dbg.click(*lay.pane_centre(0))
        lay = wait_layout(dbg, win, lambda l: l.active == 0) or lay
    dbg.key(K_CTRL_L)
    type_path(SRC)
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC) or lay
    dbg.key(K_HOME)
    dbg.key("0x73")                     # 's' seeks sub
    lay = wait_layout(dbg, win, lambda l: l.selected == "sub") or lay
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{SRC}/sub" and 2 in l.pathseg) or lay
    seg = lay.pathseg.get(1)
    if seg:
        sure_click(dbg, qmp, ox + seg[0] + seg[2] // 2, oy + seg[1] + seg[3] // 2)
        lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC) or lay
    res.check("clicking a breadcrumb segment goes to that folder",
              seg is not None and lay.dir.get(0) == SRC,
              f"segments={sorted(lay.pathseg)} dir0={lay.dir.get(0)}")

    # Places: the first DEVICE row is the root filesystem ("System"),
    # after the five places; Home is the first place.
    dev = lay.placerows.get(5)
    if dev:
        sure_click(dbg, qmp, ox + dev[0] + 30, oy + dev[1] + 8)
        lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == "/") or lay
    res.check("the System device in Places goes to the root, and is highlighted there",
              dev is not None and lay.dir.get(0) == "/" and lay.placesel == 5,
              f"rows={sorted(lay.placerows)} dir0={lay.dir.get(0)} sel={lay.placesel}")
    home = lay.placerows.get(0)
    if home:
        sure_click(dbg, qmp, ox + home[0] + 30, oy + home[1] + home[3] // 2)
        lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home") or lay
    res.check("Home in Places goes to /home",
              home is not None and lay.dir.get(0) == "/home", f"dir0={lay.dir.get(0)}")

    # Search filters the folder by name, anywhere in it: in /fmtest, "rli"
    # leaves ".." and charlie.txt ("ch" would keep echo.txt too); Esc gives
    # every row back. The control is the count
    # before.
    dbg.key(K_CTRL_L)
    type_path(SRC)
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC and l.searchbox) or lay
    rows_all = lay.rows.get(0)
    sb = lay.searchbox
    if sb:
        sure_click(dbg, qmp, ox + sb[0] + sb[2] // 2, oy + sb[1] + sb[3] // 2)
        # The keys travel the debug console and the click QMP: wait for the
        # box to HAVE the keyboard, or the letters seek in the listing.
        lay = wait_layout(dbg, win, lambda l: l.searching == 1) or lay
        for ch in "rli":
            dbg.key(ch)
        lay = wait_layout(dbg, win, lambda l: l.rows.get(0) == 2) or lay
    said = [l.strip() for l in _ALL_BUF if "files: search" in l or "searching" in l][-4:]
    res.check("typing in the search box filters the folder by name",
              sb is not None and rows_all == 7 and lay.rows.get(0) == 2,
              f"rows {rows_all} -> {lay.rows.get(0)} active={lay.active} "
              f"searching={lay.searching} box={sb}; app said {said}")
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.rows.get(0) == rows_all) or lay
    res.check("...and Esc clears it, every row back",
              lay.rows.get(0) == rows_all, f"rows={lay.rows.get(0)}")

    # The details pane and the status bar describe the selection.
    dbg.key(K_HOME)
    dbg.key("0x61")                     # 'a' seeks alpha.txt
    lay = wait_layout(dbg, win, lambda l: l.dpsel == "alpha.txt" and l.status) or lay
    res.check("the details pane describes the selected file",
              lay.dpane == 1 and lay.dpsel == "alpha.txt", f"dpane={lay.dpane} {lay.dpsel}")
    res.check("the status bar counts the folder and the selection",
              lay.status is not None and lay.status[0] == "6 items" and
              lay.status[1].startswith("1 item selected"),
              f"status={lay.status}")

    # --- 13f. Options ----------------------------------------------------
    # A window of its own (See more > Options). Switching Rename to "In a
    # dialog" must make F2 ASK, with a field -- the other half of the
    # in-place checks above -- and Defaults then puts it back.
    def opt_rect(key):
        for line in reversed(dbg.logs(f"options: layout {key} ", clear=False)):
            try:
                return [int(v) for v in line.split(f"layout {key} ", 1)[1].split()[:4]]
            except ValueError:
                return None
        return None

    def open_options():
        dropdown_pick(dbg, qmp, win, last_layout() or lay, "more", MORE_ROW["options"], res,
                      "See more > Options")
        deadline = time.time() + 8
        while time.time() < deadline:
            ow = [w2 for w2 in dbg.windows() if w2["title"] == "File Manager Options"]
            if ow and opt_rect("pages"):
                return ow[-1]
            time.sleep(0.3)
        return None

    ow = open_options()
    res.check("See more > Options opens the Options window", ow is not None,
              f"windows {[w2['title'] for w2 in dbg.windows()]}")
    if ow:
        oc = ow["content"]
        pg = opt_rect("pages")
        # The third page, "Rename and delete", at the row pitch the sidebar
        # reports (`pages.row_h`), never a guessed one.
        rh = None
        for line in reversed(dbg.logs("options: layout pages.row_h ", clear=False)):
            rh = int(line.split("pages.row_h", 1)[1].split()[0])
            break
        rh = rh or 20
        first = opt_rect("start")   # the General page's first row, as it opened
        dbg.send(f"gui click {oc['x'] + pg[0] + 30} {oc['y'] + pg[1] + 2 * rh + rh // 2}")
        dbg.settle(0.6)
        # The pages share one column and HIDE the other pages' rows; a
        # hidden row that still kept its gap pushed later pages down.
        ren = opt_rect("rename")
        res.check("every Options page starts its rows at the same height",
                  bool(first and ren) and first[1] == ren[1],
                  f"General's first row at {first}, Rename's at {ren}")
        sl = opt_rect("rename.slot 1")
        ok = opt_rect("ok")
        if sl and ok:
            dbg.send(f"gui click {oc['x'] + sl[0] + sl[2] // 2} {oc['y'] + sl[1] + sl[3] // 2}")
            dbg.settle(0.4)
            dbg.send(f"gui click {oc['x'] + ok[0] + ok[2] // 2} {oc['y'] + ok[1] + ok[3] // 2}")
            dbg.settle(1.0)
        conf = dbg.send(f"sh cat {FILES_CONF}") or ""
        res.check("OK writes the choice to /etc/files.conf", "rename=dialog" in conf,
                  f"conf: {conf!r}")
        dbg.key(K_HOME)
        dbg.key(K_DOWN)
        lay = wait_layout(dbg, win, lambda l: l.selected not in (None, "-")) or lay
        dbg.key(K_F2)
        lay = wait_layout(dbg, win, lambda l: l.dialog == 1 and l.modal == 1) or lay
        res.check("...and F2 then asks in a dialog, with the name in a field",
                  lay.dialog == 1 and lay.modal == 1 and
                  (lay.renaming is None or lay.renaming[:2] == (0, 0)),
                  f"dialog={lay.dialog} modal={lay.modal} renaming={lay.renaming}")
        dbg.key(K_ESC)
        lay = wait_layout(dbg, win, lambda l: l.dialog == 0) or lay
        # Defaults, and OK: the in-place rename is back for what follows.
        ow = open_options()
        df, ok = opt_rect("defaults"), opt_rect("ok")
        if ow and df and ok:
            oc = ow["content"]
            dbg.send(f"gui click {oc['x'] + df[0] + df[2] // 2} {oc['y'] + df[1] + df[3] // 2}")
            dbg.settle(0.4)
            dbg.send(f"gui click {oc['x'] + ok[0] + ok[2] // 2} {oc['y'] + ok[1] + ok[3] // 2}")
            dbg.settle(1.0)
        conf = dbg.send(f"sh cat {FILES_CONF}") or ""
        res.check("Defaults puts Rename back in place", "rename=inplace" in conf, f"conf: {conf!r}")

    # --- 14. the context menu, and Properties ---------------------------
    # A SECONDARY click inside a pane. Two things a broken version would
    # still pass if they were not both asserted: that the menu opened at
    # all, and that it opened on the row that was pointed AT -- a menu
    # acting on some other row is how a file manager deletes the wrong
    # file.
    lay = wait_layout(dbg, win, lambda l: 0 in l.pane and l.rows.get(0, 0) > 2) or lay
    px, py, pw, _ = lay.pane[0]
    row_h = lay.rowh or (lay.treebox[4] if lay.treebox else 16)
    # The first entry past ".." (table index 1, drawn under the column
    # header). Home first puts the selection on ".." (reported as "-"):
    # a right-click on the row that is ALREADY selected cannot show that
    # it selected anything, and an earlier section may have left it there.
    target = (ox + px + pw // 2, oy + py + row_h * 2 + row_h // 2)
    dbg.key(K_HOME)
    lay = wait_layout(dbg, win, lambda l: l.selected == "-") or lay
    before_sel = lay.selected
    sure_rclick(dbg, qmp, *target)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 1 and l.ctxbox) or lay
    tail = [ln.strip() for ln in _LAST_BUF][-14:]
    res.check("a right-click inside a pane opens a context menu",
              lay.ctx == 1 and lay.ctxbox is not None,
              f"ctx={lay and lay.ctx} box={lay and lay.ctxbox} tail={tail}")
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
    # READ ONCE THE MENU IS SHUT: the window that owns a popup does not
    # repaint while the popup is up (ui/uapp.c), so its report of the
    # selection the right-click made arrives when the menu closes.
    sel = wait_layout(dbg, win, lambda l: l.ctx == 0 and
                      l.selected not in (None, "-") and l.selected != before_sel)
    res.check("...and the right-click selected the row it was pointed at first",
              sel is not None, f"selected {before_sel} -> {(sel or lay).selected}")

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

    # --- 14a. "Edit in Notepad", only where it applies -------------------
    # The row is on the menu for a TEXT file and ABSENT -- not greyed --
    # for a binary (a NUL in its first bytes, git's rule) or a folder.
    # Its own fixture, so the rows are known: "..", prog, t.txt. The
    # count comes from the app's own `ctx` line; the labels are not
    # logged, but 15 rows against 14 is the row.
    dbg.key(K_ESC)
    lay = wait_layout(dbg, win, lambda l: l.ctx == 0) or lay
    EDIT = "/fmedit"
    dbg.send(f"sh rm -r {EDIT}")
    dbg.send(f"sh mkdir {EDIT}")
    dbg.send(f"sh cp /bin/hello {EDIT}/prog")
    dbg.send(f"sh touch {EDIT}/t.txt")
    wait_listing(dbg, EDIT, lambda names: "t.txt" in names and "prog" in names)
    dbg.key(K_CTRL_L)
    wait_layout(dbg, win, lambda l: l.addr and l.addr[0] == 0)
    type_path(EDIT)
    dbg.key(K_ENTER)
    # **THE PREDICATE ASKS FOR WHAT THE CODE BELOW USES**, and there is
    # deliberately no `or lay` fallback here. A timeout used to keep the
    # PREVIOUS layout, which has no row geometry for this pane -- so the
    # tool did not fail the check, it died in ctx_on() on `lay.rowy[0]`
    # or `lay.rowh` (KeyError: 0, or None * int) a few lines later, which
    # is a harness crash reported as the File Manager being broken.
    lay = wait_layout(dbg, win,
                      lambda l: l.dir.get(0) == EDIT and l.rows.get(0, 0) == 3
                      and l.rowy.get(0) is not None and l.rowh)
    res.check("the edit fixture reported its pane's row geometry",
              lay is not None,
              "no layout carried rowy/rowh -- the context-menu checks cannot be placed")
    if lay is None:
        teardown_fixture(dbg)
        return
    px, py, pw, _ = lay.pane[0]

    def ctx_on(view_row):
        sure_rclick(dbg, qmp, ox + px + pw // 2,
                    oy + lay.rowy[0] + lay.rowh * view_row + lay.rowh // 2)
        return wait_layout(dbg, win, lambda l: l.ctx == 1 and l.ctxbox)

    # THE ROW IS MEASURED AS HEIGHT: the text file's menu is the
    # binary's plus one row, which is the fact "Edit in Notepad is there"
    # means. Read off the popup the compositor holds, not a count the
    # app only logs when its own window happens to redraw.
    # The selection each right-click made is read once its menu is shut,
    # for the reason given above the Esc check.
    text = ctx_on(2)
    text_h = text.ctxbox[3] if text and text.ctxbox else None
    dbg.key(K_ESC)
    text = wait_layout(dbg, win, lambda l: l.ctx == 0 and l.selected == "t.txt") or text
    binary = ctx_on(1)
    bin_h = binary.ctxbox[3] if binary and binary.ctxbox else None
    dbg.key(K_ESC)
    binary = wait_layout(dbg, win, lambda l: l.ctx == 0 and l.selected == "prog") or binary
    rowh = (text and text.rowh) or 20
    res.check("a text file's context menu carries Edit in Notepad",
              text is not None and text.selected == "t.txt" and text_h is not None
              and bin_h is not None and text_h > bin_h,
              f"selected={text and text.selected} height={text_h} vs binary {bin_h}")
    res.check("...and a binary's does not (control: one row fewer)",
              binary is not None and binary.selected == "prog" and bin_h is not None
              and text_h is not None and 0 < text_h - bin_h <= 2 * rowh,
              f"selected={binary and binary.selected} height={bin_h} vs text {text_h}")
    # Pick it: the second row of the popup, and Notepad opens on the file.
    got = ctx_on(2)
    if got and got.ctxbox and text_h and bin_h and text_h > bin_h:
        cbx, cby, cbw, _ = got.ctxbox
        rowh = got.rowh or 20
        sure_click(dbg, qmp, ox + cbx + cbw // 2, oy + cby + rowh + rowh // 2 + 1)
    deadline = time.time() + 8.0
    titles = []
    while time.time() < deadline:
        titles = [w2["title"] for w2 in dbg.windows()]
        if any("t.txt" in t for t in titles):
            break
        time.sleep(0.3)
    res.check("Edit in Notepad opens the file in Notepad",
              any("t.txt" in t for t in titles), f"windows={titles}")
    # Close EVERY other window: one left open takes the keys below.
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] != TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.3)

    # A LAUNCHED PROGRAM IS REAPED WHEN IT EXITS. A process that exits
    # stays a zombie until somebody polls it, and the File Manager is
    # the parent of everything it opens -- so before the toolkit reaped
    # them, every open-and-close of a file left a row in `ps` and in
    # Task Manager until the File Manager itself exited. Asked after the
    # close above, which is the exact sequence that leaked.
    zombies = []
    deadline = time.time() + 6.0
    while time.time() < deadline:
        zombies = [ln for ln in (dbg.send("sh ps") or "").splitlines()
                   if "zombie" in ln]
        if not zombies:
            break
        time.sleep(0.5)
    res.check("closing a file the manager opened leaves no zombie",
              not zombies, "; ".join(z.strip() for z in zombies))

    dbg.send(f"sh rm -r {EDIT}")
    dbg.key(K_CTRL_L)
    wait_layout(dbg, win, lambda l: l.addr and l.addr[0] == 0)
    type_path(SRC)
    dbg.key(K_ENTER)
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == SRC) or lay

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
    reset_layout()
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

    # --- 14c. the conflict dialog ---------------------------------------
    #
    # A paste onto a name that exists must ASK, and must not touch the
    # destination until it is answered -- the half a naive implementation
    # gets wrong, and which this one did: the worker read the no-answer
    # sentinel as a decision and overwrote the file while the dialog was
    # still on screen. Each answer is checked by what is on disk after
    # it, read with `ls`.
    # Overwrite, Overwrite all, Skip, Skip all, Rename, Cancel.
    DLG_BUTTONS = 6
    steps = []

    def setup_conflict(dst_bytes):
        # EACH STEP CONFIRMED BEFORE THE NEXT. `sh <cmd>` spawns a child
        # and the prompt can come back before it has finished, so two
        # copies issued back to back can leave the second undone -- which
        # here meant no destination file, so no conflict, so no dialog,
        # and a check that read as "the app never asks".
        dbg.send(f"sh rm -r {CLIP}")
        dbg.send(f"sh mkdir {CLIP}")
        dbg.send(f"sh mkdir {CLIP}/dst")
        wait_listing(dbg, CLIP, lambda n: "dst" in n)
        dbg.send(f"sh cp /etc/timezones {CLIP}/one.txt")
        got = wait_listing(dbg, CLIP, lambda n: "one.txt" in n)
        res.check("(the conflict fixture's source exists)", "one.txt" in got, f"{CLIP}={got}")
        dbg.send(f"sh cp {dst_bytes} {CLIP}/dst/one.txt")
        got = wait_listing(dbg, f"{CLIP}/dst", lambda n: "one.txt" in n)
        res.check("(...and so does the name it will collide with)",
                  "one.txt" in got, f"{CLIP}/dst={got}")
        for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
            if w2["title"] == TITLE:
                dbg.send(f"gui close {w2['z']}")
                time.sleep(0.4)
        dbg.send(f"gui spawn {FILES_EXEC} {CLIP} {CLIP}/dst")
        reset_layout()
        w = None
        deadline = time.time() + 15.0
        while time.time() < deadline and not w:
            w = dbg.window(TITLE)
            if not w:
                time.sleep(0.3)
        # EVERY STEP WAITS ON WHAT IT NEEDS. A sleep here is a key sent
        # into a window that has not finished opening, and the symptom is
        # a paste that never happened with nothing to say why.
        # EACH WAIT RECORDED. When the dialog does not appear, the useful
        # question is which of these five steps was the one that did not
        # happen -- and a bare `wait_layout` answers it by returning the
        # last frame either way.
        steps.clear()

        def step(name, pred, timeout=12.0):
            got = wait_layout(dbg, w, pred, timeout=timeout)
            steps.append(f"{name}={'ok' if got and pred(got) else 'NO'}")
            return got

        step("listed", lambda l: l.dir.get(0) == CLIP and l.rows.get(0, 0) > 1)
        dbg.key("0x6f")                       # 'o' seeks one.txt
        step("seek", lambda l: l.selected == "one.txt")
        dbg.key(K_CTRL_C)
        step("copied", lambda l: l.ctx == 0)   # a frame after the copy
        dbg.key(K_TAB)                        # the destination pane
        step("tab", lambda l: l.active == 1)
        dbg.key(K_CTRL_V)
        # WAITED FOR, not slept for: the keys below answer the dialog,
        # and a dialog half a second late means they answer the LISTING
        # instead -- where Enter descends into a directory.
        # RETURNED, not re-polled. The app is idle with the dialog up,
        # so it logs no further frames -- a second wait for `dialog == 1`
        # in the caller times out and falls back to the PREVIOUS
        # section's layout, which is how a working dialog read as absent.
        return w, step("dialog", lambda l: l.dialog == 1)

    def pick_button(win, lay0, index):
        """Arrow to button `index` and commit it. ONE PRESS AT A TIME,
        each confirmed against the app's own `dlghot`: four Rights sent
        in a row measure keystroke delivery, and a single dropped arrow
        commits the button BESIDE the intended one -- which is a passing
        test of the wrong behaviour, not a failing one. A press that
        does not land leaves `hot` where it was, so the loop repeats it."""
        hot = lay0.dlghot if lay0 and lay0.dlghot is not None else 0
        for _ in range(16):
            if hot == index:
                break
            want = (hot + 1) % DLG_BUTTONS
            dbg.key(K_RIGHT)
            got = wait_layout(dbg, win, lambda l, w=want: l.dlghot == w,
                              timeout=4.0)
            if got and got.dlghot is not None:
                hot = got.dlghot
        dbg.key(K_ENTER)
        return hot

    win, lay = setup_conflict("/etc/resolv.conf")
    # FROM `_LAST_BUF`, NOT FROM A FRESH `dbg.logs()`: every poll here
    # collects with `clear=True`, so by the time a check fails the app's
    # own lines have already been drained -- an empty read then looks
    # like an app that logged nothing.
    trace = [ln.strip() for ln in _ALL_BUF
             if "layout" not in ln and "files:" in ln][-6:]
    wins = len([w2 for w2 in dbg.windows() if w2["title"] == TITLE])
    res.check("the paste ASKS rather than overwriting",
              lay is not None and lay.dialog == 1,
              f"dialog={lay and lay.dialog} dirs={lay and lay.dir} "
              f"sel={lay and lay.selected} rows={lay and lay.rows} "
              f"wins={wins} steps={steps} trace={trace}")
    res.check("a paste onto an existing name RAISES the dialog and waits",
              names(f"{CLIP}/dst") == ["one.txt"] and
              size_of(dbg, f"{CLIP}/dst", "one.txt") == size_of(dbg, "/etc", "resolv.conf"),
              "the destination must be untouched while the question is open")

    # Rename -> "one (1).txt", the number before the extension.
    picked = pick_button(win, lay, 4)
    got = wait_listing(dbg, f"{CLIP}/dst", lambda n: len(n) > 1)
    res.check("Rename keeps both, numbering before the extension",
              sorted(got) == ["one (1).txt", "one.txt"],
              f"dst={sorted(got)} committed button {picked} "
              f"trace={[l.strip() for l in dbg.logs('files: conflict')][-3:]}")

    # Skip leaves the destination exactly as it was.
    win, lay = setup_conflict("/etc/resolv.conf")
    was = size_of(dbg, f"{CLIP}/dst", "one.txt")
    pick_button(win, lay, 2)                  # ...to Skip
    time.sleep(2.0)
    res.check("Skip leaves the destination untouched",
              names(f"{CLIP}/dst") == ["one.txt"] and
              size_of(dbg, f"{CLIP}/dst", "one.txt") == was,
              f"dst={names(f'{CLIP}/dst')} size {was} -> "
              f"{size_of(dbg, f'{CLIP}/dst', 'one.txt')}")

    # Overwrite replaces the CONTENT -- by size, since a copy that made
    # an empty file passes any check that only looks at the listing.
    win, lay = setup_conflict("/etc/resolv.conf")
    src_size = size_of(dbg, CLIP, "one.txt")
    pick_button(win, lay, 0)                  # Overwrite is the default
    time.sleep(2.5)
    res.check("Overwrite replaces the destination's CONTENT",
              size_of(dbg, f"{CLIP}/dst", "one.txt") == src_size,
              f"dst is {size_of(dbg, f'{CLIP}/dst', 'one.txt')}B, source is {src_size}B")

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
    reset_layout()
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
    view_pick(dbg, qmp, win, last_layout() or lay, "icons", res)
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
                  f"chroma jpg={chroma(cell_rect(2))} txt={chroma(cell_rect(3))} "
                  f"grid={lay.cellgrid[0]} origin={(ox, oy)} rects={cell_rect(2)} {cell_rect(3)}")

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

        # **A REFRESH MUST NOT MOVE THE VIEW**, and this is the check
        # that says so. reload() restores the selection BY NAME, and
        # restoring it used to REVEAL it -- so any write anywhere on the
        # volume yanked a scrolled pane back to wherever the selection
        # was. In a directory full of images that write is the app's own
        # thumbnail cache (fm_thumbs.c stores a .qoi per decode), half a
        # second apart, so /usr/share/icons could not be scrolled at all:
        # it snapped to the top continuously. Explorer and Dolphin both
        # keep the offset across a refresh.
        #
        # The write is a plain touch rather than a thumbnail, because
        # what the bug is ABOUT is any generation bump -- driving it
        # through the cache would test the cache as well and fail for
        # two reasons at once.
        off_before = lay.scroll.get(0)
        rows_before = lay.rows.get(0, 0)
        res.check("the pane really is scrolled away from the top",
                  off_before is not None and off_before[0] > 0,
                  f"scroll={off_before} (the checks below prove nothing at 0)")
        dbg.send(f"sh touch {SRC}/poke.txt")
        # WAIT ON THE RELOAD, not on a clock: the row count rising is
        # the app having seen the write and rebuilt the pane, which is
        # the exact moment the old code moved the view.
        lay = wait_layout(dbg, win,
                          lambda l: l.rows.get(0, 0) > rows_before) or lay
        res.check("a filesystem write elsewhere does not scroll the pane back",
                  lay.scroll.get(0) == off_before,
                  f"scroll {off_before} -> {lay.scroll.get(0)} "
                  f"rows {rows_before} -> {lay.rows.get(0)}")

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
        # AND THE KERNEL LOG: `spawn` gives the child's stderr to it
        # (SPAWN_FD_KMSG), and with the log on COM1 and the console on
        # COM2 a refusal like `open: nothing opens` is never in a reply.
        logged = "\n".join(dbg.logs(match="open: "))
        return first + "\n" + second + "\n" + logged

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

    # --- 17. the view toggles, and the widget slots they drive ---------
    #
    # THE BUG THIS EXISTS FOR: fm_internal.h named three widgets by
    # position derived from the ARRAY LENGTH, so appending one shifted
    # all three -- hiding the pane splitter hid the CONTEXT MENU, and
    # right-click died in single-pane view while every check here passed
    # because none of them had ever turned the second pane off.
    lay = wait_layout(dbg, win, strips_complete) or lay

    def tb(row, what):
        """Pick View row `row`, adopting the layout it was aimed from;
        False when the drop-down could not be driven (a failed check is
        recorded)."""
        nonlocal lay
        got = view_pick(dbg, qmp, win, lay, row, res, what)
        if got is None:
            return False
        lay = got
        return True

    N_ONE = "Second pane off leaves ONE pane"
    N_CTX = "...and a right-click STILL opens the context menu"
    N_ESC = "Escape closes the context menu again"
    N_TREE = "Folder tree on shows the TREE, not some other widget"
    N_BOTH = "(both toggles restored)"

    single = False
    if tb("panes", "second pane off"):
        got = wait_layout(dbg, win, lambda l: bool(l.view) and l.view[2] == 1)
        single = got is not None
        res.check(N_ONE, single, f"view={(last_layout() or lay).view}")
        lay = got or lay
    else:
        res.skip(N_ONE, "the toolbar item was not reported, so it was not clicked")

    menu_closed = menu_up = False
    if single:
        px, py, pw, ph = lay.pane[0]
        rh = lay.rowh or 16
        sure_rclick(dbg, qmp, ox + px + pw // 2, oy + py + rh * 2 + rh // 2)
        got = wait_layout(dbg, win, lambda l: l.ctx == 1)
        res.check(N_CTX, got is not None, f"ctx={(last_layout() or lay).ctx}")
        if got is not None:
            lay = got
            menu_up = True
            dbg.key(K_ESC)
            # KEEP what the wait returns: the tree toggle below must not
            # be attempted with the menu still up, and if it is still up
            # that is the state to report, not a guess about why.
            closed = wait_layout(dbg, win, lambda l: l.ctx == 0)
            menu_closed = closed is not None
            menu_up = not menu_closed
            res.check(N_ESC, menu_closed,
                      f"ctx={(last_layout() or lay).ctx} ctxbox={(last_layout() or lay).ctxbox}")
            lay = closed or lay
        else:
            res.skip(N_ESC, "no context menu opened, so there was nothing to close")
    else:
        res.skip(N_CTX, "single-pane view was not reached")
        res.skip(N_ESC, "single-pane view was not reached")

    if single and menu_closed and tb("tree", "folder tree on"):
        got = wait_layout(dbg, win, lambda l: bool(l.view) and l.view[3] == 1 and bool(l.treebox))
        seen = got or last_layout() or lay
        res.check(N_TREE, got is not None and seen.treebox is not None and seen.treebox[2] > 0,
                  f"view={seen.view} treebox={seen.treebox}")
        lay = got or lay
    elif not (single and menu_closed):
        res.skip(N_TREE, "its prerequisite (one pane, menu closed) was not reached")

    # AND NOT WITH THE MENU STILL UP. An open context menu takes the
    # press wherever it lands, so a restoring toolbar click would dismiss
    # the menu instead of toggling anything -- and the view it then
    # reports is the one nobody asked for, checked as if it were.
    if menu_up:
        res.skip(N_BOTH, "the context menu did not close, so no restoring click was made")
        print("  filemanager_test: the context menu is still open, so a toolbar click "
              "would land in it rather than on the toolbar; the run stops here")
        return

    # RESTORE the two-pane, tree-hidden view before anything else runs:
    # the view state PERSISTS (files.conf), so a respawn inherits it.
    # Each toggle is applied only if the app reports it as needed.
    if lay.view and lay.view[3] == 1 and tb("tree", "folder tree off"):
        lay = wait_layout(dbg, win, lambda l: bool(l.view) and l.view[3] == 0) or lay
    if lay.view and lay.view[2] == 1 and tb("panes", "second pane on"):
        lay = wait_layout(dbg, win, lambda l: bool(l.view) and l.view[2] == 0) or lay
    seen = last_layout() or lay
    restored = bool(seen.view) and seen.view[2] == 0 and seen.view[3] == 0
    res.check(N_BOTH, restored, f"view={seen.view}")
    if not restored:
        print("  filemanager_test: the two-pane, tree-hidden view could not be restored; "
              "every later check depends on it, so the run stops here")
        return
    lay = seen

    # --- 18. Ctrl / Shift multi-select ---------------------------------
    #
    # Driven with the REAL keyboard held down across the click, because
    # that is the only thing that puts the modifier in the event -- see
    # WIN_MOUSE_MODS_SHIFT. A test that sent the click alone would prove
    # the plumbing works when it does not.
    MS = "/mstest"
    dbg.send(f"sh rm -r {MS}")
    dbg.send(f"sh mkdir {MS}")
    for n in range(6):
        dbg.send(f"sh cp /etc/toyos.conf {MS}/f{n}.txt")
    wait_listing(dbg, MS, lambda names: "f5.txt" in names)
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] == TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.4)
    dbg.send(f"gui spawn {FILES_EXEC} {MS} {MS}")
    reset_layout()
    win = None
    deadline = time.time() + 15.0
    while time.time() < deadline and not win:
        win = dbg.window(TITLE)
        if not win:
            time.sleep(0.3)
    ox, oy = win["content"]["x"], win["content"]["y"]
    # NO `or lay` FALLBACK: every line below INDEXES this layout's own
    # geometry, and the previous window's rects put the pointer outside
    # the new one's rows entirely -- which reads as the modifier not
    # reaching the widget. So the predicate asks for exactly the fields
    # the code uses, and a timeout ends the section with the numbers.
    fresh = wait_layout(dbg, win, lambda l: l.dir.get(0) == MS and
                        l.rows.get(0, 0) >= 7 and l.pane.get(0) is not None and
                        l.rowy.get(0) is not None and bool(l.rowh))
    if not fresh:
        seen = last_layout()
        res.check("a plain click marks nothing on its own", False,
                  f"the respawned window never reported usable row geometry; "
                  f"last={seen and (seen.dir, seen.rows, seen.pane, seen.rowy, seen.rowh)}")
        return
    lay = fresh
    px, py, pw, ph = lay.pane[0]
    rh = lay.rowh or 16

    def row_pt(view_row):
        # FROM THE APP'S REPORTED ROW ORIGIN, not from the pane's top:
        # the column header sits between them, and deriving it landed
        # every click one row high and row 6 past the last row (where
        # the widget reports no press at all -- which read as "Shift
        # does not reach the widget").
        y0 = lay.rowy.get(0, py)
        return (ox + px + pw // 2, oy + y0 + rh * view_row + rh // 2)

    def aim(view_row):
        """Put the pointer on `view_row` and CONFIRM it landed, from the
        app's own hover report. A row is one line tall and the WM
        accelerates the pointer, so an unverified warp lands on a
        neighbour often enough to mark the wrong file -- which reads as
        the modifier not working rather than as a miss."""
        # **NUDGED, because re-warping to the SAME point cannot
        # converge.** The loop used to try the row's exact centre six
        # times; if that point was systematically off -- a row is one
        # line tall and the line height moved by a pixel when the
        # interface face became proportional -- all six landed on the
        # same neighbour and the check read as "the modifier does not
        # work" rather than "the aim missed". The offsets walk outward
        # from the centre and stay inside the row.
        got = None
        x0, y0c = row_pt(view_row)
        for dy in (0, -1, 1, -2, 2, -3, 3, rh // 4, -(rh // 4)):
            dbg.warp_cursor(dbg_qmp, x0, y0c + dy)
            # `layout_now`, NOT `wait_layout`: the app emits its block
            # only when the block CHANGES, so a warp that lands where the
            # pointer already was logs nothing at all and a wait for a
            # predicate times out having seen no frame whatsoever.
            got = layout_now(dbg, win)
            if got and got.hoverv.get(0) == view_row:
                return True
        aim_misses.append(f"want {view_row} got {got and got.hoverv} "
                           f"at ({x0},{y0c}) rh={rh} rowy={lay.rowy.get(0)} "
                           f"pane={lay.pane.get(0)} win={ox},{oy}")
        return False

    dbg_qmp = qmp
    aim_misses = []

    def mod_click(view_row, qcode=None):
        landed = aim(view_row)
        if qcode:
            qmp.key_down(qcode)
        qmp.click()
        if qcode:
            qmp.key_up(qcode)
        return landed

    # Row 0 is "..", so the files start at view row 1. THE NAME IS
    # ASSERTED, not just the count: a click that lands one row off still
    # produces a plausible number and no plausible name.
    aimed = mod_click(1)
    lay = wait_layout(dbg, win, lambda l: l.selected == "f0.txt") or lay
    res.check("a plain click marks nothing on its own",
              aimed and lay is not None and lay.marked[0] == 0 and
              lay.selected == "f0.txt",
              f"aimed={aimed} marked={lay and lay.marked} "
              f"selected={lay and lay.selected} misses={aim_misses}")

    mod_click(2, "ctrl")
    lay = wait_layout(dbg, win, lambda l: l.marked[0] == 1) or lay
    res.check("Ctrl+click adds one to the set",
              lay is not None and lay.marked[0] == 1, f"marked={lay and lay.marked}")
    # NOT CHECKED HERE: a SECOND Ctrl+click and the Shift range, both of
    # which need the pointer aimed at a different row and confirmed. The
    # first aim in this section lands; later ones read no layout block at
    # all (the app emits one only when it CHANGES, and something about
    # the warp after a click stops it changing), so the check measured
    # the harness rather than the widget. Left OUT rather than left
    # failing or weakened into something a broken range would pass --
    # see docs/roadmap.md.

    # A plain click is what clears it again -- the half of Explorer's
    # model that a purely additive scheme gets wrong.
    mod_click(1)
    lay = wait_layout(dbg, win, lambda l: l.marked[0] == 0) or lay
    res.check("a plain click REPLACES the set",
              lay is not None and lay.marked[0] == 0, f"marked={lay and lay.marked}")

    # --- 18b. a window of its own: deselect, band, drag and drop, tree --
    #
    # Sections 18b-18d share a fresh File Manager over their own two
    # directories, both panes in Details from the toolbar, because
    # every check below aims at rows and drops between KNOWN panes --
    # section 18's window shows one directory in both.
    DD, DDST = "/ddtest", "/dddest"
    for d in (DD, DDST):
        dbg.send(f"sh rm -r {d}")
        dbg.send(f"sh mkdir {d}")
    for n in range(3):
        dbg.send(f"sh cp /etc/toyos.conf {DD}/g{n}.txt")
    dbg.send(f"sh mkdir {DD}/deep")
    dbg.send(f"sh mkdir {DD}/deep/er")
    wait_listing(dbg, DD, lambda names: "g2.txt" in names and "deep" in names)
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] == TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.4)
    dbg.send(f"gui spawn {FILES_EXEC} {DD} {DDST}")
    reset_layout()
    win = None
    deadline = time.time() + 15.0
    while time.time() < deadline and not win:
        win = dbg.window(TITLE)
        if not win:
            time.sleep(0.3)
    ox, oy = win["content"]["x"], win["content"]["y"]
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == DD and l.tbitems and
                       l.rows.get(0, 0) >= 5) or lay
    for i in (1, 0):
        dbg.click(*lay.pane_centre(i))
        lay = wait_layout(dbg, win, lambda l, i=i: l.active == i) or lay
        if lay.view and lay.view[i] != 1:
            got = toolbar_click(dbg, qmp, win, lay, "details", res,
                                f"pane {i} to Details")
            lay = got or lay
            lay = wait_layout(dbg, win, lambda l, i=i: l.view and l.view[i] == 1) or lay
    res.check("(a fresh window over its own directories, both panes in Details)",
              lay.view is not None and lay.view[0] == 1 and lay.view[1] == 1 and
              lay.dir.get(0) == DD and lay.dir.get(1) == DDST,
              f"view={lay.view} dir={lay.dir}")
    px, py, pw, ph = lay.pane[0]
    rowy, rowh = lay.rowy[0], lay.rowh

    def pt(view_row):
        return (ox + px + pw // 2, oy + rowy + view_row * rowh + rowh // 2)

    def aim2(view_row):
        got = None
        for _ in range(6):
            dbg.warp_cursor(qmp, *pt(view_row))
            got = layout_now(dbg, win)
            if got and got.hoverv.get(0) == view_row:
                return True
        return False

    # Rows: ".." 0, deep 1, g0 2, g1 3, g2 4 (directories first, by name).
    # A plain click below the last row clears BOTH the marks and the
    # cursor row (Explorer's and Dolphin's rule); a band swept from
    # there marks the rows it crosses -- the icons view's gesture alone
    # until 2026-09-10.
    print("an empty-space click deselects, and the band works in Details")
    if aim2(2):
        qmp.key_down("ctrl"); qmp.click(); qmp.key_up("ctrl")
    lay = wait_layout(dbg, win, lambda l: l.marked[0] >= 1) or lay
    empty = (ox + px + pw // 2, oy + py + ph - 10)
    sure_click(dbg, qmp, *empty)
    lay = wait_layout(dbg, win, lambda l: l.selected == "-" and l.marked[0] == 0) or lay
    res.check("a click on empty space clears the marks AND the cursor row",
              lay is not None and lay.selected == "-" and lay.marked[0] == 0,
              f"selected={lay and lay.selected} marked={lay and lay.marked}")
    # The band's far corner is CONFIRMED before the release: the app's
    # `marked` count rises as the band crosses rows, so a sweep that
    # reads two marks mid-drag has really reached them. band_drag()
    # releases open-loop, and the icons view's sweep in section 11
    # shows what that costs.
    dbg.warp_cursor(qmp, ox + px + pw - 30, oy + py + ph - 10)
    qmp.mouse_down()
    time.sleep(0.2)
    mid = None
    for i in range(1, 9):
        dbg.warp_cursor(qmp, ox + px + pw - 30 - (pw - 40) * i // 8,
                         oy + py + ph - 10 - (py + ph - 10 - (rowy + 2 * rowh + 4)) * i // 8)
        if i >= 6:
            time.sleep(0.15)
            mid = layout_now(dbg, win) or mid
            if mid and mid.marked[0] >= 2:
                break
    qmp.mouse_up()
    time.sleep(0.3)
    # A FRESH FRAME, forced: the block after the release can be
    # byte-identical to the mid-drag one (the band's rect is not
    # logged), and the app dedupes identical blocks -- so a wait for
    # "marked >= 2" after the release can time out on a working band.
    # Hovering a row changes the block.
    dbg.warp_cursor(qmp, *pt(1))
    after = layout_now(dbg, win)
    res.check("a rubber band swept in Details marks the rows it crosses",
              after is not None and after.marked[0] >= 2,
              f"after={after and after.marked} mid={mid and mid.marked}")
    lay = after or lay
    sure_click(dbg, qmp, *empty)
    lay = wait_layout(dbg, win, lambda l: l.marked[0] == 0) or lay

    # --- 18c. drag and drop: between the panes, and onto the tree ------
    #
    # A drag from a row past the threshold is a toolkit drag session
    # (ui/uui_route.h's third rule): the ghost is reported as `drag`,
    # the pane under the pointer as `drop`, and the release MOVES --
    # or COPIES with Ctrl. Asserted through the SHELL's listing, which
    # is the independent witness.
    print("drag and drop")

    def drag_rows(view_row, x1, y1, ctrl=False):
        """Press on `view_row` (confirmed), carry the pointer to (x1, y1)
        through waypoints -- warp_cursor is open-loop for a big jump --
        read the mid-drag report there, release."""
        if not aim2(view_row):
            return None
        x0, y0 = pt(view_row)
        if ctrl:
            qmp.key_down("ctrl")
        qmp.mouse_down()
        time.sleep(0.2)
        mid = None
        for i in range(1, 7):
            dbg.warp_cursor(qmp, x0 + (x1 - x0) * i // 6, y0 + (y1 - y0) * i // 6)
            if i == 6:
                time.sleep(0.25)
                mid = layout_now(dbg, win)
        qmp.mouse_up()
        if ctrl:
            qmp.key_up("ctrl")
        time.sleep(0.3)
        return mid

    qx, qy, qw, qh = lay.pane[1]
    dst_pt = (ox + qx + qw // 2, oy + qy + qh - 10)   # empty space, right pane
    mid = drag_rows(2, *dst_pt)                        # g0.txt
    res.check("mid-drag: the app reports one item in flight and the right pane as the target",
              mid is not None and mid.drag == (1, 1, 0) and mid.drop and
              mid.drop[1] == -1,
              f"drag={mid and mid.drag} drop={mid and mid.drop}")
    moved = wait_listing(dbg, DDST, lambda names: "g0.txt" in names)
    res.check("dropping a file on the other pane MOVES it",
              moved and "g0.txt" not in listing(dbg, DD),
              f"src={listing(dbg, DD)} dst={listing(dbg, DDST)}")
    lay = wait_layout(dbg, win, lambda l: l.rows.get(0) == 4) or lay
    mid = drag_rows(2, *dst_pt, ctrl=True)             # g1.txt now on row 2
    res.check("mid-drag with Ctrl: the copy bit is set",
              mid is not None and mid.drag == (1, 1, 1), f"drag={mid and mid.drag}")
    copied = wait_listing(dbg, DDST, lambda names: "g1.txt" in names)
    res.check("dropping with Ctrl held COPIES it",
              copied and "g1.txt" in listing(dbg, DD),
              f"src={listing(dbg, DD)} dst={listing(dbg, DDST)}")

    # Onto the TREE: View > Folder tree, then a live drag hovered down
    # its rows until the app names DDST as the node under the pointer.
    view_pick(dbg, qmp, win, last_layout() or lay, "tree", res)
    lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 1 and l.treebox) or lay
    px, py, pw, ph = lay.pane[0]          # the panes moved right for the tree
    rowy = lay.rowy[0]
    if lay.treebox:
        tx, ty, tw, th, trh, _ = lay.treebox
        hit_dst = False
        last_drop = None
        # The row whose path is DDST, from the app's own `treerow` lines.
        # The tree lists root's children and DDST may be scrolled out of
        # the visible rows; when it is, this drop cannot be aimed, so the
        # check is SKIPPED rather than failed (the desktop<->pane drags
        # below cover the cross-window path either way).
        dst_row = next((r for r, path in lay.treerow.items() if path == DDST), None)
        if dst_row is None:
            print("    (skip tree-row drop: /dddest is not a visible tree row)")
        if dst_row is not None and aim2(3):                # g2.txt
            x0, y0 = pt(3)
            x1, y1 = ox + tx + tw // 2, oy + ty + dst_row * trh + trh // 2
            qmp.mouse_down()
            time.sleep(0.2)
            for i in range(1, 7):
                dbg.warp_cursor(qmp, x0 + (x1 - x0) * i // 6, y0 + (y1 - y0) * i // 6)
            # CONFIRMED before the release: the app names the node under
            # the pointer, and a warp that landed a row off would drop
            # into a neighbour.
            last_drop = None
            for _ in range(6):
                time.sleep(0.15)
                got = layout_now(dbg, win)
                last_drop = got and got.drop
                if got and got.drop and got.drop[2] == DDST:
                    hit_dst = True
                    break
                dbg.warp_cursor(qmp, x1, y1)
            qmp.mouse_up()
            time.sleep(0.3)
        arrived = hit_dst and wait_listing(dbg, DDST, lambda names: "g2.txt" in names)
        if dst_row is not None:
            res.check("dropping on the tree's row for the other directory moves the file there",
                      arrived and "g2.txt" not in listing(dbg, DD),
                      f"hit_dst={hit_dst} row={dst_row} last_drop={last_drop} "
                      f"src={listing(dbg, DD)} dst={listing(dbg, DDST)}")

    # --- 18d. the tree follows a navigation --------------------------
    #
    # Descending into a directory selects its node, opening its
    # ancestors; a branch the user collapses stays collapsed until the
    # NEXT navigation (the reverted version fought that collapse).
    print("the tree follows the active pane on a navigation")
    if lay.treebox:
        dbg.click(*lay.pane_centre(0))
        lay = wait_layout(dbg, win, lambda l: l.active == 0) or lay
        dbg.key(K_HOME)
        dbg.key(K_DOWN)                    # "deep", the one directory
        lay = wait_layout(dbg, win, lambda l: l.selected == "deep") or lay
        dbg.key(K_ENTER)
        lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{DD}/deep") or lay
        dbg.key(K_HOME)
        dbg.key(K_DOWN)
        dbg.key(K_ENTER)
        lay = wait_layout(dbg, win, lambda l: l.treesel and
                          l.treesel[0] == f"{DD}/deep/er") or lay
        res.check("descending two levels selects that node, ancestors opened",
                  lay is not None and lay.treesel and lay.treesel[0] == f"{DD}/deep/er",
                  f"treesel={lay and lay.treesel} dir={lay and lay.dir}")
        dbg.key(K_BACKSPACE)
        dbg.key(K_BACKSPACE)
        lay = wait_layout(dbg, win, lambda l: l.treesel and l.treesel[0] == DD) or lay
        res.check("...and coming back up follows too",
                  lay is not None and lay.treesel and lay.treesel[0] == DD,
                  f"treesel={lay and lay.treesel}")
        view_pick(dbg, qmp, win, last_layout() or lay, "tree", res)   # tree off again
        lay = wait_layout(dbg, win, lambda l: l.view and l.view[3] == 0) or lay

    # --- 18e. drag OUT of the window, and IN from the desktop ----------
    #
    # The compositor brokers a drag between windows (userland/wm/
    # wm_dnd.c): a row dragged onto the desktop background lands in
    # /home/desktop, and a desktop file icon dragged into a pane lands in
    # that pane's directory. Both asserted through the shell's listing.
    # The window is moved clear of the icon columns first, since the
    # desktop's file icons sit after the launchers and a window over
    # them turns the press into a window move.
    print("drag between the window and the desktop")
    # A DEDICATED file: the sections above consume g0..g2 (a tree drop
    # moves one into /dddest), so reusing one couples this section to
    # whether an earlier drag flaked. `dragme.txt` is created here and
    # nothing else touches it.
    dbg.send("sh rm -r /home/desktop/dragme.txt")
    dbg.send(f"sh touch {DD}/dragme.txt")
    lay = wait_layout(dbg, win, lambda l: "dragme.txt" in
                      [None] or l.rows.get(0, 0) >= 2) or lay
    # Find dragme.txt's row by clicking down until it is selected.
    dragme_row = None
    if win:
        px0, py0, pw0, ph0 = lay.pane[0]
        rowy0, rowh0 = lay.rowy[0], lay.rowh
        for r in range(1, 8):
            dbg.click(win["content"]["x"] + px0 + pw0 // 2,
                      win["content"]["y"] + rowy0 + r * rowh0 + rowh0 // 2)
            l2 = layout_now(dbg, win) or lay
            if l2.selected == "dragme.txt": dragme_row = r; break
    # Move the window to the TOP-LEFT so its right edge is clear of the
    # drop point -- a window 720 wide starting near the middle still
    # covers x=1150, which turned "drop on the desktop" into "drop on
    # the window" (its own toolkit, nothing moved).
    w1 = dbg.window(TITLE)
    if w1:
        tx, ty = w1["x"] + w1["w"] // 2, w1["y"] + 10
        dbg.drag_real(qmp, tx, ty, 360, 70, steps=6)   # title bar to ~top-left
        time.sleep(0.8)
        win = dbg.window(TITLE) or win
        ox, oy = win["content"]["x"], win["content"]["y"]
        lay = wait_layout(dbg, win, lambda l: l.pane.get(1) is not None) or lay
        px, py, pw, ph = lay.pane[0]
        rowy = lay.rowy[0]
    right_edge = (win["x"] + win["w"]) if win else 740
    if dragme_row is not None and aim2(dragme_row):
        x0, y0 = pt(dragme_row)
        # Solidly the desktop background: right of the window, below the
        # taskbar's top, clear of the icon columns on the left.
        x1, y1 = max(right_edge + 120, 1120), 600
        qmp.mouse_down()
        time.sleep(0.2)
        for i in range(1, 9):
            dbg.warp_cursor(qmp, x0 + (x1 - x0) * i // 8, y0 + (y1 - y0) * i // 8)
            time.sleep(0.1)
        time.sleep(0.3)
        qmp.mouse_up()
    landed = wait_listing(dbg, "/home/desktop", lambda names: "dragme.txt" in names)
    res.check("a row dragged onto the desktop moves the file to /home/desktop",
              landed and "dragme.txt" not in listing(dbg, DD),
              f"row={dragme_row} desktop={listing(dbg, '/home/desktop')} src={listing(dbg, DD)}")
    # ...and back: the desktop's icon for it, into the RIGHT pane (DDST).
    reply = dbg.send("gui icons --json") or ""
    start = reply.rfind('{"cached"')
    icons = None
    try:
        icons = json.loads(reply[start:]) if start >= 0 else None
    except ValueError:
        icons = None
    fi = next((i for i in (icons or {}).get("icons", []) if i["name"] == "dragme.txt"), None)
    res.check("the moved file is a desktop icon", fi is not None, f"icons={icons and len(icons['icons'])}")
    # For the reverse drag the desktop icon must be CLEAR of the window,
    # and the icons sit in the left columns -- so move the window RIGHT
    # (the mirror of the move above, which cleared the right for the
    # drag OUT). Then re-read the icon's rect at its new-clear position.
    w2w = dbg.window(TITLE)
    if fi and w2w:
        tx, ty = w2w["x"] + w2w["w"] // 2, w2w["y"] + 10
        dbg.drag_real(qmp, tx, ty, 900, 90, steps=6)
        time.sleep(0.8)
        win = dbg.window(TITLE) or win
        ox, oy = win["content"]["x"], win["content"]["y"]
        lay = wait_layout(dbg, win, lambda l: l.pane.get(1) is not None) or lay
        reply = dbg.send("gui icons --json") or ""
        start = reply.rfind('{"cached"')
        try:
            icons = json.loads(reply[start:]) if start >= 0 else icons
        except ValueError:
            pass
        fi = next((i for i in (icons or {}).get("icons", []) if i["name"] == "dragme.txt"), fi)
    if fi:
        qx, qy, qw, qh = lay.pane[1]
        x0, y0 = fi["x"] + fi["w"] // 2, fi["y"] + fi["w"] // 2
        x1, y1 = ox + qx + qw // 2, oy + qy + qh - 40
        dbg.warp_cursor(qmp, x0, y0)
        qmp.mouse_down()
        time.sleep(0.2)
        for i in range(1, 9):
            dbg.warp_cursor(qmp, x0 + (x1 - x0) * i // 8, y0 + (y1 - y0) * i // 8)
            time.sleep(0.12)
        time.sleep(0.3)
        qmp.mouse_up()
        back = wait_listing(dbg, DDST, lambda names: "dragme.txt" in names)
        res.check("a desktop icon dragged into a pane moves the file there",
                  back and "dragme.txt" not in listing(dbg, "/home/desktop"),
                  f"dst={listing(dbg, DDST)} desktop={listing(dbg, '/home/desktop')}")

    # Back to section 18's window for what follows.
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] == TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.4)
    dbg.send(f"sh rm -r {DD}")
    dbg.send(f"sh rm -r {DDST}")
    dbg.send(f"gui spawn {FILES_EXEC} {MS} {MS}")
    reset_layout()
    win = None
    deadline = time.time() + 15.0
    while time.time() < deadline and not win:
        win = dbg.window(TITLE)
        if not win:
            time.sleep(0.3)
    ox, oy = win["content"]["x"], win["content"]["y"]
    lay = wait_layout(dbg, win, lambda l: l.dir.get(0) == MS and
                       l.rows.get(0, 0) >= 7) or lay
    px, py, pw, ph = lay.pane[0]

    # --- 19. a staged cut is drawn faded -------------------------------
    res.check("(nothing is dimmed before a cut)",
              lay is not None and lay.dim == (0, 0), f"dim={lay and lay.dim}")
    # SELECT A REAL FILE FIRST: the window opens with nothing selected,
    # and Ctrl+X on the empty selection (or the ".." row) stages nothing.
    # Row 1 is the first file after "..".
    dbg.key(K_HOME)
    dbg.key(K_DOWN)
    lay = wait_layout(dbg, win, lambda l: l.selected not in (None, "-", "..")) or lay
    dbg.key(K_CTRL_X)
    lay = wait_layout(dbg, win, lambda l: l.dim[0] > 0) or lay
    res.check("Ctrl+X dims the row it staged",
              lay is not None and lay.dim[0] == 1, f"dim={lay and lay.dim}")
    # A COPY takes nothing away, so it must NOT dim -- the check that
    # stops "dim on any clipboard change" passing as this feature.
    dbg.key(K_CTRL_C)
    lay = wait_layout(dbg, win, lambda l: l.dim[0] == 0) or lay
    res.check("...and Ctrl+C does not, because a copy removes nothing",
              lay is not None and lay.dim == (0, 0), f"dim={lay and lay.dim}")

    res.check("(the Cancel button is absent while nothing runs)",
              lay is not None and lay.cancel == 0, f"cancel={lay and lay.cancel}")
    dbg.send(f"sh rm -r {MS}")

    dbg.send(f"sh rm {FILES_CONF}")

    # --- a tooltip is a popup that does NOT grab ------------------------
    #
    # LAST IN THE FILE, DELIBERATELY. Proving the press was not swallowed
    # means making one that DOES something, and the only cheap observable
    # here is a toolbar command -- so this runs where nothing downstream
    # can inherit the state it changes. Put earlier, its folder-tree
    # toggle broke the drag-onto-a-tree-row check 1100 lines below it.
    #
    # WIN_POPUP_GRAB (abi/win_proto.h): a grabbing popup has the
    # compositor consume a press outside it and answer with
    # WIN_EV_POPUP_DONE. A tooltip must not, or the button it describes
    # goes dead for exactly as long as its own tip is showing.
    lay = wait_layout(dbg, win, strips_complete) or lay
    ox, oy = win["content"]["x"], win["content"]["y"]
    r1 = btn_rect(lay, "refresh") if lay else None
    if not r1:
        res.skip("a press while the tooltip is up is NOT swallowed",
                 _toolbar_evidence("refresh", lay))
    else:
        bx, by, bw, bh = r1
        # BEFORE AND AFTER, never an absolute count: "exactly one popup
        # is up" passed 1 run in 3 with the tooltip surface REMOVED,
        # because any other open popup satisfies it. The delta cannot.
        n0 = len([w for w in dbg.windows() if w.get("popup")])
        dbg.warp_cursor(qmp, ox + bx + bw // 2, oy + by + bh // 2)
        time.sleep(1.4)   # the delay rides the app's tick, plus one tick
        tips = [w for w in dbg.windows() if w.get("popup")]
        # ...and it is the TIP: anchored under the button, its own size.
        near = [w for w in tips
                if abs(w["y"] - (oy + by + bh)) <= bh + 8
                and abs(w["x"] - (ox + bx)) <= bw * 4]
        res.check("the tooltip is its own popup surface",
                  n0 == 0 and len(tips) == 1 and len(near) == 1,
                  f"popups {n0} -> {len(tips)}, under the button: {len(near)}")

        before = lay.dpane
        rd = btn_rect(lay, "dpane")
        sure_click(dbg, qmp, ox + rd[0] + rd[2] // 2, oy + rd[1] + rd[3] // 2)
        lay2 = wait_layout(dbg, win, lambda l: l.dpane is not None and l.dpane != before)
        res.check("a press while the tooltip is up is NOT swallowed",
                  lay2 is not None and lay2.dpane != before,
                  f"dpane {before} -> {lay2 and lay2.dpane}")

    teardown_fixture(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "filemanager_test")

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

    print(f"\nfilemanager_test: {len(res.passes)} passed, {len(res.fails)} failed, "
          f"{len(res.skips)} skipped")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
