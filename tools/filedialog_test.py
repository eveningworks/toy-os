#!/usr/bin/env python3
"""tools/filedialog_test.py -- the shared file chooser, as a window.

Drives Notepad's, Image Viewer's and Audio Player's Open dialogs
(userland/ui/uui_filedialog.c) and asks the compositor and the widget
what they did, rather than reading pixels: the chooser reports its own
geometry under the `filedialog:` prefix (ui/uapp.h's
uapp_window_desc.log_prefix) and `gui windows --json` now says whether a
window is a dialog, which one owns it, and whether that owner is blocked.

What it proves, and what a broken version would still pass:

  * Ctrl-O opens a SEPARATE TOPLEVEL, not a box inside the app: the
    compositor lists a second window with `dialog: true`, an `owner`
    naming the app's own slot, and CHROME (its content origin is below
    its frame). A ui/uui_dialog modal drawn in the app's own window --
    which is what Notepad had -- lists no second window at all.
  * It carries NO TASKBAR BUTTON. An owned window does not on Win32
    either, and the strip listing one is what a naive "it is just a
    window" implementation gives you.
  * MODALITY: a press on the owner raises the DIALOG instead of the
    owner. Without it the owner comes to the front and the modal ends up
    behind the window it is modal to.
  * A PLACES row navigates: clicking "Root" lists `/`, which the
    starting directory was not.
  * A ROUND TRIP: pick a real file in the chooser, and Notepad's title
    and text area show that file. The chooser is not asserted on at all
    here -- the evidence is the app having loaded what was chosen.
  * ESCAPE cancels: the dialog window is gone and the owner has focus
    back (so the modal block was really lifted, not merely hidden).
  * CLOSING THE OWNER takes its dialog with it -- the leak an
    independent toplevel would leave on screen with nothing to close it.
  * Image Viewer and Audio Player open the SAME chooser, reporting under
    the same prefix. Both used to have no chooser at all.
  * A "Files of type" combo is present and FILTERS: Image Viewer's
    chooser lists fewer rows on "Image files" than on "All files", in a
    directory that holds both. A combo that merely drew would pass an
    "is it there" check and fail this one.
  * NOTHING in the owner responds: not its menu bar (primary click),
    not its context menu (SECONDARY click), and it does not light up
    under the cursor. The WM blocked only the primary button at first.
  * A FILTERED chooser still lists FOLDERS. Every app's filter says no
    to a directory -- correctly, for a sidebar pinned to one folder --
    so the chooser has to override it, or the root lists nothing and
    there is no way out. That shipped; see check_navigable().
  * The two form labels are the SAME width and vertically CENTRED on
    their controls -- the label rect is taller than one text row and its
    midline sits within a pixel of the field's. Top-aligned labels (the
    first version) fail the second half.

Positive controls, each run once when this was written (2026-09-14):
  * `on_dialog_created()` returning 0 (no dialog granted): NOTHING is
    green -- the run stops at the first group with 5 of 5 red.
  * `wm_dialog_blocker()` returning -1 always reddens both modality
    checks and, as a knock-on, Places: the owner ends up over the
    chooser, so later clicks land on the wrong window.
  * `unlisted()` in wm_taskbar.c reverted to `windows[i].popup` reddens
    exactly the taskbar check.

What the second control TAUGHT, because the first two attempts at the
modality check stayed green under it: the check has to press a point on
the owner's title bar that is neither under the chooser (which is wider
than the window that opened it) nor on the app ICON (a press there opens
the window menu instead of dragging). Both of those make "the owner did
not move" true with the block removed entirely. clear_grab() picks the
point, and place_owner() pins the window first so such a point exists.

Usage (the VM must already be up):

    python3 tools/vm.py --instance auto start
    python3 tools/filedialog_test.py --instance <n>
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402

NOTEPAD = "/bin/wm/apps/notepad"
IMGVIEW = "/bin/wm/apps/imgview"
PLAYER = "/bin/wm/apps/player"
CTRL_O = "0x0f"
CTRL_S = "0x13"   # Save As, the mode where the name field is focused
ESC = "0x1b"
# A fixture that is on every image and is not in the directory any of the
# three apps starts in, so "the app shows this" cannot be true already.
PICK_DIR = "/tests"
PICK_NAME = "sample.txt"
# Holds images and non-images both, so the two filters cannot agree by
# accident -- see check_filter().
MIXED_DIR = "/tests"


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        if not ok and detail:
            print(f"        {detail}")


# --- asking, rather than guessing -------------------------------------

def windows(dbg):
    return dbg.json("gui windows --json").get("windows", [])


def dialog(dbg):
    """The TOPMOST dialog window, or None. Topmost rather than first:
    a run that left an earlier app open would otherwise be answered
    about a window nothing in this test is driving."""
    for w in reversed(windows(dbg)):
        if w.get("dialog"):
            return w
    return None


def owner_of(dbg, dlg):
    for w in windows(dbg):
        if not w.get("dialog") and w.get("client_pid") == dlg.get("client_pid"):
            return w
    return None


def wait_dialog(dbg, want=True, timeout=6.0):
    deadline = time.time() + timeout
    while True:
        d = dialog(dbg)
        if (d is not None) == want or time.time() >= deadline:
            return d
        time.sleep(0.05)


def fd_geom(dbg, name):
    """The chooser's own rect for one named item, content-relative."""
    for line in reversed(dbg.logs(f"filedialog: layout {name} ", clear=False)):
        parts = line.split()
        try:
            return tuple(int(x) for x in parts[-4:])
        except ValueError:
            return None
    return None


def name_text(dbg):
    """What the File name field says, as the chooser itself reports it."""
    lines = dbg.logs('filedialog: layout name.text ', clear=False)
    if not lines:
        return None
    tail = lines[-1].split('name.text ', 1)[1].strip()
    return tail[1:-1] if len(tail) >= 2 and tail[0] == '"' else tail


def click_in(dbg, dlg, x, y):
    """A click at a point in the DIALOG's content coordinates."""
    dbg.click(dlg["content"]["x"] + x, dlg["content"]["y"] + y)
    dbg.settle()


def place_owner(dbg, w, x=8, y=8):
    """Move a freshly spawned window to a KNOWN corner.

    The cascade puts each new window a little further right, so where
    the app lands depends on what an earlier run left open -- and the
    chooser is centred and wide enough to swallow a window that started
    far enough in. The modality check then has nowhere on the owner's
    title bar to press and fails as a fixture problem that reads exactly
    like a regression. Pinning it is one drag."""
    icon = w.get("icon") or {}
    grab = w["x"] + max(24, icon.get("size", 0) + 12)
    dbg.drag(grab, w["y"] + 4, grab - (w["x"] - x), w["y"] + 4 - (w["y"] - y))
    dbg.settle()


def open_chooser(dbg, path, title, timeout=8.0):
    """Spawn an app, pin it where the chooser cannot cover all of it,
    and press Ctrl-O. Returns (owner, dialog)."""
    dbg.spawn(path, title=title)
    dbg.settle()
    ws = [w for w in windows(dbg) if w.get("client_pid")]
    if ws:
        place_owner(dbg, ws[-1])
    dbg.key(CTRL_O, mods="ctrl")
    dlg = wait_dialog(dbg, True, timeout)
    dbg.settle()
    return (owner_of(dbg, dlg) if dlg else None), dlg


def close_all(dbg):
    for w in windows(dbg):
        if w.get("client_pid"):
            dbg.send(f"sh kill {w['client_pid']}")
    dbg.settle()


# --- the checks --------------------------------------------------------

def check_window(dbg, res):
    owner, dlg = open_chooser(dbg, NOTEPAD, "untitled")
    if not dlg:
        res.check("Ctrl-O opens a dialog WINDOW", False, "no dialog window listed")
        return None, None
    res.check("Ctrl-O opens a dialog WINDOW", True)
    # The owner is named by its client WINDOW id, which for a uapp
    # toplevel is slot 0 -- and both rows must be the same process, or
    # the compositor has owned this dialog to somebody else's window.
    res.check("...owned by the app's own window",
              owner is not None and dlg.get("owner") == 0 and
              owner.get("client_pid") == dlg.get("client_pid"),
              f"owner={dlg.get('owner')} "
              f"pids {owner.get('client_pid') if owner else None}/{dlg.get('client_pid')}")
    res.check("...and it is MODAL", dlg.get("modal") is True)
    # Chrome: a popup's content starts at its own origin; a real window's
    # is inset by the border and the title bar.
    has_chrome = dlg["content"]["y"] > dlg["y"] and dlg["content"]["x"] > dlg["x"]
    res.check("...with a title bar and a border", has_chrome,
              f"frame y={dlg['y']} content y={dlg['content']['y']}")
    res.check("...titled for the action", dlg.get("title") == "Open",
              f"title {dlg.get('title')!r}")

    bar = dbg.json("gui taskbar --json")
    titles = [b.get("label", "") for b in bar.get("buttons", [])]
    res.check("the dialog has NO taskbar button of its own",
              "Open" not in titles, f"taskbar {titles}")
    return owner, dlg


def clear_grab(owner, dlg):
    """A point on the owner's title bar that is neither under the dialog
    nor on the app icon.

    Both exclusions were found by the positive control. The chooser is
    bigger than the window that opened it and lands over its middle, so
    "press the centre of the title bar" presses the DIALOG; and a press
    on the app icon opens the window menu rather than starting a drag.
    Either one makes "the owner did not move" true with the modal block
    removed entirely -- a check measuring nothing."""
    icon = owner.get("icon") or {}
    ix0 = icon.get("x", -1)
    ix1 = ix0 + icon.get("size", 0)
    for x in range(owner["x"] + 4, owner["x"] + owner["w"] - 6, 4):
        if (x < dlg["x"] or x >= dlg["x"] + dlg["w"]) and not (ix0 <= x < ix1 + 4):
            return x
    return None


def check_modal(dbg, res, owner, dlg):
    """A press on the owner must be SWALLOWED, and the dialog stay on top.

    Two checks. The second is the one that measures the block: the
    dialog ending up on top is ALSO what the taskbar's raise rule does,
    so "the dialog is on top" alone would survive the block being
    removed. Dragging discriminates -- blocked, the window does not move
    at all."""
    grab = clear_grab(owner, dlg)
    if grab is None:
        res.check("the blocked owner cannot even be DRAGGED", False,
                  "the dialog covers the owner's whole title bar")
        return

    # THE DRAG FIRST, and the plain press after it -- in that order,
    # because a press on the same point within the double-click window
    # maximizes instead of dragging, which is a third way for this check
    # to be true for the wrong reason.
    dbg.drag(grab, owner["y"] + 4, grab + 60, owner["y"] + 64)
    dbg.settle()
    after = owner_of(dbg, dialog(dbg))
    res.check("the blocked owner cannot even be DRAGGED",
              after is not None and after["x"] == owner["x"] and after["y"] == owner["y"],
              f"grabbed at x={grab}: {(owner['x'], owner['y'])} -> "
              f"{(after['x'], after['y']) if after else None}")

    # RE-READ the owner: the drag above must not have moved it, but a
    # BROKEN build moves it -- and pressing the old coordinates would
    # then press empty desktop and leave this check green for a third
    # wrong reason.
    time.sleep(0.5)   # past TITLE_DOUBLE_CLICK_TICKS
    now, d2 = owner_of(dbg, dialog(dbg)), dialog(dbg)
    if not now or not d2:
        res.check("a press on the owner leaves the DIALOG on top", False, "owner gone")
        return
    grab = clear_grab(now, d2)
    if grab is None:
        res.check("a press on the owner leaves the DIALOG on top", False,
                  "no clear point on the owner's title bar")
        return
    dbg.click(grab, now["y"] + 4)
    dbg.settle()
    top = windows(dbg)[-1]
    res.check("a press on the owner leaves the DIALOG on top",
              top.get("dialog") is True and top.get("focused") is True,
              f"top is {top.get('title')!r} dialog={top.get('dialog')}")


def fd_int(dbg, part):
    """One integer the chooser reports about itself (`places.row_h`,
    `view.rows`). Asking beats deriving a font-derived number here."""
    for line in reversed(dbg.logs(f"filedialog: layout {part} ", clear=False)):
        try:
            return int(line.split(part, 1)[1].split()[0])
        except (IndexError, ValueError):
            return None
    return None


def place_rows(dbg):
    """Each Places row's rect, as the widget reports it (`places.row i x y
    w h`), in order. The rows are NOT one pitch -- a device is taller and
    a caption sits between the groups -- so a test clicks these rather
    than stepping by a row height."""
    rows = {}
    for line in dbg.logs("filedialog: layout places.row ", clear=False):
        parts = line.split("places.row", 1)[1].split()
        try:
            rows[int(parts[0])] = tuple(int(v) for v in parts[1:5])
        except (IndexError, ValueError):
            continue
    return [rows[i] for i in sorted(rows)]


def fd_dir(dbg):
    """Which directory the chooser is listing, as the widget reports it."""
    for line in reversed(dbg.logs("filedialog: layout view.dir ", clear=False)):
        return line.split("view.dir", 1)[1].strip()
    return None


def check_places(dbg, res, dlg):
    """A Places row navigates. Driven by the strip's own rect and the
    row pitch the sidebar derives from the font, and asserted on the
    DIRECTORY the view reports -- not on the rows having moved, which a
    reload that went nowhere would also do."""
    places = fd_geom(dbg, "places")
    start = fd_dir(dbg)
    if not places or start is None:
        res.check("a Places row navigates the listing", False,
                  f"places={places} dir={start}")
        return
    px, py, pw, ph = places
    del pw
    # Walk the strip top to bottom until the listed directory changes.
    # The strip SKIPS destinations that do not resolve (uui_filedialog.c),
    # so a fixed row index would name a different place on a different
    # image -- the one thing a test must not assume.
    moved = None
    for rx, ry, rw, rh in place_rows(dbg):
        y = ry + rh // 2
        if y > py + ph - 4:
            break
        click_in(dbg, dlg, rx + 20, y)
        now = fd_dir(dbg)
        if now and now != start:
            moved = now
            break
    res.check("a Places row navigates the listing", moved is not None,
              f"started in {start!r}, never moved")
    if moved:
        res.check("...to a directory that really exists",
                  moved.startswith("/"), f"listed {moved!r}")


def check_pathbar(dbg, res, dlg):
    """The breadcrumb: its ROOT segment goes to "/", and a click past the
    segments gives a text field whose Enter navigates -- and does NOT
    commit the dialog, which Enter anywhere else would."""
    seg = None
    for line in reversed(dbg.logs("filedialog: layout path.seg 0 ", clear=False)):
        try:
            seg = tuple(int(v) for v in line.split("path.seg 0", 1)[1].split()[:4])
        except ValueError:
            seg = None
        break
    if seg:
        click_in(dbg, dlg, seg[0] + seg[2] // 2, seg[1] + seg[3] // 2)
    res.check("the breadcrumb's root segment lists the root",
              seg is not None and fd_dir(dbg) == "/", f"seg={seg} dir={fd_dir(dbg)!r}")
    path = fd_geom(dbg, "path")
    if not path:
        res.check("a path typed into the breadcrumb navigates, and the dialog stays",
                  False, "no path rect")
        return
    click_in(dbg, dlg, path[0] + path[2] - 8, path[1] + path[3] // 2)
    for ch in "/usr/share":
        dbg.key("0x2f" if ch == "/" else ch)
    dbg.key("0x0a")
    dbg.settle()
    res.check("a path typed into the breadcrumb navigates, and the dialog stays",
              fd_dir(dbg) == "/usr/share" and dialog(dbg) is not None,
              f"dir={fd_dir(dbg)!r} dialog={dialog(dbg) is not None}")


def check_owner_inert(dbg, res, owner, dlg):
    """NOTHING in the owner responds while the chooser is up.

    Three doors, because a modal that only blocks one is not a modal:
    the primary click (its menu bar), the SECONDARY click (its context
    menu), and MOTION -- hover is the only thing that tells a person a
    control is live, so an owner that still highlighted under the cursor
    would be advertising controls it refuses."""
    ox, oy = owner["content"]["x"], owner["content"]["y"]
    # Notepad's menu bar is the top row of its content area.
    dbg.logs("notepad: action", clear=True)
    dbg.click(ox + 20, oy + 6)          # File
    dbg.settle()
    dbg.rclick(ox + 60, oy + 40)        # its text area
    dbg.settle()
    acted = dbg.logs("notepad: action", clear=False)
    res.check("the owner's menu bar takes no click",
              not acted, f"notepad acted on {acted}")

    # And the dialog is still the one on top after both.
    top = windows(dbg)[-1]
    res.check("...nor does a secondary click reach it",
              top.get("dialog") is True,
              f"top is {top.get('title')!r}")
    del dlg


def check_navigable(dbg, res, dlg):
    """A FILTERED chooser can still be walked.

    Every app's filter answers "is this a file I can open", so every one
    of them says no to a directory -- and a chooser that honoured that
    literally lists an empty root with no way out of it. That shipped,
    and 22 green checks missed it because each reached its directory by
    a Places row or a typed path instead of by WALKING one. So: go to
    the root, require folders to be listed, and open one by activating
    the row rather than by naming it."""
    view = fd_geom(dbg, "view")
    if not view:
        res.check("a filtered chooser still lists folders", False, "no view rect")
        return
    # The root, via the System device row, then count what is listed.
    places = fd_geom(dbg, "places")
    if places:
        for rx, ry, rw, rh in place_rows(dbg):
            if ry + rh // 2 > places[1] + places[3] - 4:
                break
            click_in(dbg, dlg, rx + 20, ry + rh // 2)
            if fd_dir(dbg) == "/":
                break
    res.check("a filtered chooser still lists folders",
              fd_dir(dbg) == "/" and (fd_int(dbg, "view.rows") or 0) > 1,
              f"dir={fd_dir(dbg)!r} rows={fd_int(dbg, 'view.rows')}")


def check_labels(dbg, res):
    """The two form labels line up and sit CENTRED on their controls."""
    nl, nf = fd_geom(dbg, "namelabel"), fd_geom(dbg, "name")
    tl = fd_geom(dbg, "typelabel")
    if not nl or not nf:
        res.check("the form labels are centred on their fields", False,
                  f"namelabel={nl} name={nf}")
        return
    res.check("both form labels are the same width",
              tl is not None and tl[2] == nl[2], f"name={nl[2]} type={tl[2] if tl else None}")
    # CENTRED, not merely "not at the top": compare MIDLINES, which is
    # the thing that was wrong -- a label box one text row tall sitting
    # at the top of a taller row.
    lmid = nl[1] + nl[3] // 2
    fmid = nf[1] + nf[3] // 2
    res.check("...and vertically centred on the field",
              abs(lmid - fmid) <= 1 and nl[3] >= nf[3] - 1,
              f"label y={nl[1]} h={nl[3]} (mid {lmid}); field y={nf[1]} h={nf[3]} (mid {fmid})")


def check_filter(dbg, res):
    """The type combo is there AND changes what is listed.

    Driven to a directory holding BOTH images and non-images first:
    Image Viewer opens in /usr/share/wallpapers, where every file is an
    image and both filters legitimately give the same answer -- which is
    the check passing for a reason that has nothing to do with filtering
    (it did, the first time this was written)."""
    owner, dlg = open_chooser(dbg, IMGVIEW, None)
    del owner
    if not dlg:
        res.check("the chooser has a Files-of-type combo", False, "no dialog")
        return
    combo = fd_geom(dbg, "type")
    res.check("the chooser has a Files-of-type combo", combo is not None,
              "no `type` rect reported")
    name = fd_geom(dbg, "name")
    if not combo or not name:
        return

    # Into a MIXED directory, by typing it: OK on a folder enters it.
    click_in(dbg, dlg, name[0] + 10, name[1] + name[3] // 2)
    for ch in MIXED_DIR:
        dbg.key({"/": "0x2f"}.get(ch, ch), settle=False)
    dbg.settle()
    dbg.key("0x0d")
    dbg.settle()
    listed = fd_dir(dbg)
    if listed != MIXED_DIR:
        res.check("picking All files lists MORE than the image filter", False,
                  f"could not reach {MIXED_DIR}; chooser is in {listed!r}")
        dbg.key(ESC)
        wait_dialog(dbg, False)
        return
    narrow = fd_int(dbg, "view.rows")

    # A PRINTABLE KEY SEEKS THE VALUE, as a Windows or KDE combobox does
    # (ui/uui_dropdown.h) -- deterministic, where clicking a popup row
    # means deriving its rect from the box's.
    click_in(dbg, dlg, combo[0] + combo[2] // 2, combo[1] + combo[3] // 2)
    dbg.key("a")
    dbg.key("0x0d")
    dbg.settle()
    picked = fd_int(dbg, "type.selected")
    wide = fd_int(dbg, "view.rows")
    res.check("the combo commits the row that was seeked", picked == 1,
              f"type.selected={picked}")
    res.check("picking All files lists MORE than the image filter",
              narrow is not None and wide is not None and wide > narrow,
              f"in {MIXED_DIR}: image filter listed {narrow} rows, "
              f"All files listed {wide}")
    dbg.key(ESC)
    wait_dialog(dbg, False)


def check_roundtrip(dbg, res, dlg):
    """Type a real path and commit: the app must load that file."""
    name = fd_geom(dbg, "name")
    if not name:
        res.check("a chosen file reaches the app", False, "no name field reported")
        return
    nx, ny, nw, nh = name
    click_in(dbg, dlg, nx + 10, ny + nh // 2)
    for ch in f"{PICK_DIR}/{PICK_NAME}":
        dbg.key({"/": "0x2f", ".": "0x2e", "_": "0x5f"}.get(ch, ch), settle=False)
    dbg.settle()
    dbg.key("0x0d")          # Return commits
    wait_dialog(dbg, False)
    dbg.settle()
    res.check("the chooser closes on commit", dialog(dbg) is None)
    title = ""
    for w in windows(dbg):
        if not w.get("dialog") and w.get("client_pid"):
            title = w.get("title", "")
    res.check("a chosen file reaches the app",
              PICK_NAME in title, f"window title {title!r}")


def check_places_hover(dbg, res, qmp):
    """THE POINTER MUST NOT KILL THE NAME FIELD BY RESTING ON PLACES.

    The regression: `case ID_PLACES` had no `reason` guard, so a mouse
    MOTION over the strip re-listed the directory and re-inited the name
    field -- and uui_textbox_init() clears `active` while the focus ring
    still points AT the field, so no click could revive it. Save As was
    unusable with the pointer anywhere over Places, which is what made
    the clipboard tool's three disk round trips fail.

    THE POINTER MUST BE THERE BEFORE THE DIALOG OPENS. A dbg.move() onto
    an already-open chooser does NOT reproduce it, and a version of this
    check that did that passed against the unfixed code -- so the order
    here is the whole check. It also has to be SAVE rather than Open:
    Open focuses the list, and the dead field is invisible from there.

    Two assertions, one per defect, because either fix alone satisfies
    the other's.
    """
    # Open once to learn where the strip lands, then close and aim there.
    owner, dlg = open_chooser(dbg, NOTEPAD, "untitled")
    places = fd_geom(dbg, "places") if dlg else None
    if not dlg or not places:
        res.check("a hover over Places leaves Save As usable", False,
                  f"dlg={dlg is not None} places={places}")
        close_all(dbg)
        return
    px, py, pw, ph = places
    sx = dlg["content"]["x"] + px + pw // 2
    sy = dlg["content"]["y"] + py + ph // 2
    dbg.key(ESC)
    wait_dialog(dbg, False)

    # THE REAL CURSOR, warped and confirmed. `gui move` (dbg.move) is
    # one wm_run() iteration and the driver snaps the pointer back on
    # the next, so a version of this check using it passed against the
    # unfixed chooser -- the motion never persisted to the open.
    dbg.warp_cursor(qmp, sx, sy)
    dbg.settle()
    dbg.key(CTRL_S, mods="ctrl")   # Save As -- the name field is focused
    dlg = wait_dialog(dbg, True)
    dbg.settle()
    if not dlg:
        res.check("a hover over Places leaves Save As usable", False,
                  "Ctrl-S opened no dialog")
        close_all(dbg)
        return
    for ch in "zq":
        dbg.key(ch, settle=False)
    dbg.settle()
    res.check("Save As takes typing with the pointer over Places",
              name_text(dbg) == "zq", f"the field holds {name_text(dbg)!r}")
    dbg.warp_cursor(qmp, sx + 4, sy + 2)   # a further hover must not wipe it
    dbg.settle()
    res.check("...and a hover does not clear what was typed",
              name_text(dbg) == "zq", f"the field holds {name_text(dbg)!r}")
    dbg.key(ESC)
    wait_dialog(dbg, False)
    close_all(dbg)


def check_cancel(dbg, res):
    owner, dlg = open_chooser(dbg, NOTEPAD, "untitled")
    if not dlg:
        res.check("Escape cancels the chooser", False, "no dialog opened")
        return
    dbg.key(ESC)
    wait_dialog(dbg, False)
    dbg.settle()
    res.check("Escape cancels the chooser", dialog(dbg) is None)
    top = windows(dbg)[-1]
    res.check("...and the owner has the focus back",
              top.get("focused") is True and not top.get("dialog"),
              f"top {top.get('title')!r}")


def check_owner_close(dbg, res):
    owner, dlg = open_chooser(dbg, NOTEPAD, "untitled")
    if not dlg or not owner:
        res.check("closing the owner takes its dialog with it", False, "no dialog opened")
        return
    dbg.send(f"sh kill {owner['client_pid']}")
    wait_dialog(dbg, False)
    dbg.settle()
    res.check("closing the owner takes its dialog with it", dialog(dbg) is None,
              f"{[w.get('title') for w in windows(dbg)]}")


def check_other_apps(dbg, res):
    for path, title, label in ((IMGVIEW, None, "Image Viewer"),
                               (PLAYER, None, "Audio Player")):
        dbg.spawn(path, title=title)
        dbg.settle()
        dbg.key(CTRL_O, mods="ctrl")
        dlg = wait_dialog(dbg, True)
        dbg.settle()
        res.check(f"{label} opens the same chooser",
                  dlg is not None and dlg.get("dialog") is True,
                  f"windows {[w.get('title') for w in windows(dbg)]}")
        if dlg:
            dbg.key(ESC)
            wait_dialog(dbg, False)
        close_all(dbg)


def run(dbg, res, qmp):
    dbg.send("sh config set desktop.layout_log on")
    close_all(dbg)

    print("the dialog is a window")
    owner, dlg = check_window(dbg, res)
    if dlg:
        print("modality")
        check_modal(dbg, res, owner, dlg)
        print("places")
        check_places(dbg, res, dialog(dbg))
        print("the breadcrumb")
        check_pathbar(dbg, res, dialog(dbg))
        print("the owner is inert")
        check_owner_inert(dbg, res, owner_of(dbg, dialog(dbg)) or owner, dialog(dbg) or dlg)
        print("walking a filtered chooser")
        check_navigable(dbg, res, dialog(dbg) or dlg)
        print("the form rows")
        check_labels(dbg, res)
        print("the round trip")
        check_roundtrip(dbg, res, dialog(dbg) or dlg)
    close_all(dbg)

    print("a hover must not disturb Save As")
    check_places_hover(dbg, res, qmp)
    close_all(dbg)

    print("cancelling")
    check_cancel(dbg, res)
    close_all(dbg)

    print("the owner going away")
    check_owner_close(dbg, res)
    close_all(dbg)

    print("the type filter")
    check_filter(dbg, res)
    close_all(dbg)

    print("the other two apps")
    check_other_apps(dbg, res)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "filedialog_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, res, qmp)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nfiledialog_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
