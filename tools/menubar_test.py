#!/usr/bin/env python3
"""tools/menubar_test.py -- the menu bar and status bar, against ring-3 Notepad.

WHAT THIS COVERS
----------------
`userland/ui/uui_menubar.c` and `userland/ui/uui_statusbar.c`, driven
through the only app that uses them. Fifteen checks, grouped:

  * The bar and its popups are DRAWN, not merely responsive. The ring-3
    Calculator shipped with no visible buttons past a green suite
    because every check asserted that clicking one changed something --
    which it did (docs/gui-guidelines.md, "three ways a GUI test passes
    without testing anything"). So the popup's own background is
    sampled, against a control point that must NOT change.
  * The Windows/KDE interaction rules: a title opens on PRESS, an item
    commits on RELEASE, a press dragged off commits NOTHING, a click
    outside dismisses AND is swallowed rather than reaching the text.
  * Nesting: hovering a submenu parent opens a second level, placed to
    the RIGHT of its parent, and Esc closes ONE level rather than the
    whole chain.
  * State asked for, not stored: Save is greyed while the document is
    clean and releasing on it commits nothing; Recent files is greyed
    until a file has been saved and opens a real submenu afterwards.
  * The status bar's panes: the indicator changes when the cursor moves
    while the message pane does not -- "it changed" paired with
    something that must not, since either half alone is satisfied by a
    bug.

GEOMETRY COMES FROM THE APP
--------------------------
Every rectangle is read from Notepad's own `notepad: layout ...` lines
(menu, menu.title N, menu.popup LEVEL, menu.item LEVEL INDEX, status,
status.pane N -- the toolkit's own describe vocabulary now that the bars
are routed widgets, not the app's private names).
Re-deriving a menu's rectangles in Python would be hopeless anyway --
they depend on which submenu is open and on which way the placement
flipped -- but the rule holds regardless: four tools have been bitten by
a derived copy drifting silently (docs/gui-guidelines.md).

POSITIVE CONTROL
----------------
Two that were run when this was written, and which say which checks are
load-bearing:

  * Make `uui_menubar_press()` commit instead of `uui_menubar_release()`
    (act on button-down). The CANCEL check goes red and NOTHING ELSE
    does -- every other check clicks and releases in the same place, so
    they cannot tell the two apart. That one check is the whole of the
    press-then-commit rule's enforcement here.
  * Have `uui_menubar_press()` return 0 for a click outside an open
    menu. The dismiss check stays GREEN -- the menu still closes -- and
    only the click-through check goes red, because the caret moves to
    where the dismissing click landed. That is why the caret is measured
    as well as the popup.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui
from qmp_test import QMPSession
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SPAWN_PATH = "/bin/wm/apps/notepad"   # spawned directly -- see spawn()
SPAWN_TIMEOUT_S = 20.0
ENTER = "0x0d"
ESC = "0x1b"
F10 = "0xa4"        # KEY_F10 -- api/keyboard.h
DOWN = "0x92"       # KEY_ARROW_DOWN
RIGHT = "0x96"      # KEY_ARROW_RIGHT

# uui_menubar.c's palette, and uui_statusbar.c's.
POPUP_BG = (250, 250, 252)
BAR_BG = (235, 235, 238)

# Menu command codes, from userland/gui/notepad.c. A test asserting on
# the app's action log needs the same numbering the app commits.
CMD_SAVE = 3
CMD_GOTO_TOP = 13
CMD_GOTO_END = 14
CMD_STATUSBAR = 15

# Numbered lines, never identical ones: moving identical content is
# pixel-identical, which is how a scroll test once reported a working
# feature as broken (docs/gui-guidelines.md).
SAMPLE_LINES = ["line one", "line two", "line three"]

SAVE_NAME = "menutest.txt"


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


class Layout:
    """Notepad's self-reported rectangles, content-relative.

    Keys are the layout line's words after `notepad: layout`, joined --
    "menu", "menu.title 0", "menu.popup 1", "menu.item 0 3", "status.pane 2".

    ONLY THE MOST RECENT DRAW is parsed, and that matters more than it
    sounds. The log accumulates, and a `popup 1` line from a submenu that
    has since closed stays in it forever -- so a parser taking the last
    occurrence of each key reports every menu that has EVER been open as
    still open. Written that way first, it turned four real passes into
    failures and, worse, would have turned a menu that never closed into
    a pass. Notepad's log_layout() emits `layout scrollbar` first on
    every draw, so that line is the frame boundary.
    """

    def __init__(self, lines, content):
        self.r = {}
        self.cx, self.cy = content["x"], content["y"]
        last = -1
        for i, line in enumerate(lines):
            if "notepad: layout scrollbar" in line:
                last = i
        if last >= 0:
            lines = lines[last:]
        for line in lines:
            if "notepad: layout " not in line:
                continue
            parts = line.split("notepad: layout ", 1)[1].split()
            if len(parts) < 5:
                continue
            what = parts[0]
            nums = []
            for p in parts[1:]:
                try:
                    nums.append(int(p))
                except ValueError:
                    nums = []
                    break
            if len(nums) < 4:
                continue
            key = " ".join([what] + [str(n) for n in nums[:-4]])
            self.r[key] = tuple(nums[-4:])

    def has(self, key):
        return key in self.r

    def rect(self, key):
        """Screen-coordinate (x, y, w, h)."""
        x, y, w, h = self.r[key]
        return (self.cx + x, self.cy + y, w, h)

    def centre(self, key):
        x, y, w, h = self.rect(key)
        return (x + w // 2, y + h // 2)

    def popups(self):
        return sorted(int(k.split()[1]) for k in self.r if k.startswith("menu.popup "))


def npwin(dbg):
    for w in dbg.json("gui windows --json").get("windows", []):
        t = w.get("title", "").lstrip("*")
        if t.rsplit("/", 1)[-1] in ("untitled", "notepad", SAVE_NAME, "/" + SAVE_NAME):
            return w
    return None


def layout(dbg, content):
    return Layout(dbg.logs("notepad: layout", clear=False), content)


def actions(dbg, clear=True):
    out = []
    for line in dbg.logs("notepad: action", clear=clear):
        try:
            out.append(int(line.split("notepad: action", 1)[1].split()[0]))
        except (IndexError, ValueError):
            pass
    return out


def wait_layout(dbg, content, ok, timeout=5.0):
    """Poll the app's self-reported layout until ok(Layout) holds, or
    timeout. Layout reads only the LATEST frame (from the last
    'layout scrollbar' boundary), so as the app redraws after an action
    the state converges -- an observable wait in place of a fixed sleep.
    Returns the final Layout either way, so the caller's own check still
    runs (and fails with detail) on a timeout."""
    deadline = time.time() + timeout
    while True:
        lay = layout(dbg, content)
        if ok(lay) or time.time() >= deadline:
            return lay
        time.sleep(0.03)


def wait_actions(dbg, timeout=4.0):
    """Poll until the app logs a committed action, then return the list
    (consumed). A menu commit is exactly one action, so non-empty is
    done; clear=False leaves them intact for the final consuming read."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if actions(dbg, clear=False):
            return actions(dbg)
        time.sleep(0.03)
    return actions(dbg)


def unpark(dbg, qmp):
    """Move the real cursor off any menu, to empty desktop.

    The companion to hover(): a parked cursor is the point of that
    helper and a hazard everywhere else. Top-left of the SCREEN, which
    is desktop icons at worst -- hovering an icon changes nothing that
    this tool measures, where hovering a menu row changes everything.
    """
    dbg.warp_cursor(qmp, 8, 8)
    dbg.settle()


def hover(dbg, qmp, rect):
    """Park the REAL cursor over `rect` and leave it there.

    NOT `gui move`: injected input overrides the mouse for ONE WM
    iteration and then the real pointer takes over, so a submenu opened
    by an injected hover closes again before the layout can be read.
    That raced, and it is the whole story behind this tool's long-
    standing intermittent failure -- the check saw no submenu perhaps one
    run in three, while a second identical hover opened it every time.

    warp_cursor() drives the actual PS/2 cursor and confirms arrival, so
    the hover PERSISTS. Same rule dialog_test.py already follows for
    hover states (docs/gui-guidelines.md, CLAUDE.md).
    """
    dbg.warp_cursor(qmp, rect[0] + rect[2] // 2, rect[1] + rect[3] // 2)
    dbg.settle()


def shot(qmp, tmp, name):
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def region(im, rect):
    x, y, w, h = rect
    return im.crop((x, y, x + w, y + h)).tobytes()


def typ(dbg, text):
    for ch in text:
        dbg.send(f"gui key {'0x20' if ch == ' ' else ch}")
    dbg.settle()


def spawn(dbg, res):
    # `gui spawn`, not a Terminal typing `run notepad`: the kernel-space
    # Terminal retired in M41's stage 0, and the ring-3 one has no window
    # yet when the injected keys would arrive.
    dbg.send(f"gui spawn {SPAWN_PATH}")

    deadline = time.time() + SPAWN_TIMEOUT_S
    while time.time() < deadline:
        win = npwin(dbg)
        if win:
            return win
        time.sleep(0.3)
    res.check("Notepad runs as a ring-3 client with its own window", False)
    return None


def run(dbg, qmp, tmp, res):
    win = spawn(dbg, res)
    if not win:
        return
    res.check("Notepad runs as a ring-3 client with its own window", True)

    for i, line in enumerate(SAMPLE_LINES):
        typ(dbg, line)
        if i < len(SAMPLE_LINES) - 1:
            dbg.send(f"gui key {ENTER}")
    dbg.settle()

    win = npwin(dbg)
    content = win["content"]

    # --- 1. the app reports what it drew -------------------------------
    want = ["menu", "menu.title 0", "menu.title 1", "menu.title 2", "status",
            "status.pane 0", "status.pane 1", "status.pane 2"]
    lay = wait_layout(dbg, content, lambda l: all(l.has(k) for k in want))
    missing = [k for k in want if not lay.has(k)]
    res.check("Notepad reports its menu bar and status bar geometry",
              not missing, f"missing: {missing}")
    if missing:
        return

    # --- 2. the bar is DRAWN, not merely present -----------------------
    im = shot(qmp, tmp, "mb_rest.png")
    bx, by, bw, bh = lay.rect("menu")
    strip = [im.getpixel((x, by + bh // 2)) for x in range(bx + 2, bx + bw - 2, 3)]
    ink = sum(1 for p in strip if p != BAR_BG)
    res.check("the menu bar strip is painted and has titles in it",
              ink > 0 and any(p == BAR_BG for p in strip),
              f"{ink} non-background samples of {len(strip)} across the strip")

    # --- 3. a title opens on PRESS, and reads as pressed ---------------
    t0 = lay.centre("menu.title 0")   # File
    t1c = lay.rect("menu.title 1")    # Edit -- the control point
    before_t1 = region(im, t1c)
    before_t0 = region(im, lay.rect("menu.title 0"))

    dbg.click(*t0)
    dbg.settle()
    lay2 = wait_layout(dbg, content, lambda l: l.popups() == [0])
    im2 = shot(qmp, tmp, "mb_file.png")

    res.check("clicking a title opens its menu", lay2.popups() == [0],
              f"open popup levels: {lay2.popups()}")
    res.check("the open title reads as pressed, and its neighbour does not",
              region(im2, lay.rect("menu.title 0")) != before_t0
              and region(im2, t1c) == before_t1,
              "half the assertion is Edit staying put")

    if lay2.popups() != [0]:
        return

    # --- 4. the popup is really painted --------------------------------
    px, py, pw, ph = lay2.rect("menu.popup 0")
    # The popup's own background, sampled clear of any label: a column
    # just inside its right border, down the middle of the first row.
    ix, iy, iw, ih = lay2.rect("menu.item 0 0")
    bgpix = im2.getpixel((px + pw - 3, iy + ih // 2))
    outside = im2.getpixel((px + pw + 12, iy + ih // 2))
    res.check("the popup is drawn (its own background reaches the screen)",
              bgpix == POPUP_BG and outside != POPUP_BG,
              f"inside {bgpix} (want {POPUP_BG}), outside {outside}")

    # --- 5. a disabled item is visibly different, and does not commit --
    # File > Save is item 4, greyed while the document is clean... except
    # it is dirty now (text was typed), so use the RECENT submenu, which
    # is empty until something has been saved.
    dis = lay2.rect("menu.item 0 2")   # Recent files
    ena = lay2.rect("menu.item 0 1")   # Open...
    row_ink = lambda r: sum(1 for x in range(r[0] + 4, r[0] + r[2] - 4)
                            if im2.getpixel((x, r[1] + r[3] // 2)) != POPUP_BG)
    res.check("a disabled item is drawn differently from an enabled one",
              row_ink(dis) > 0 and row_ink(dis) != row_ink(ena),
              f"disabled row ink {row_ink(dis)}, enabled row ink {row_ink(ena)}")

    actions(dbg)
    dbg.click(dis[0] + dis[2] // 2, dis[1] + dis[3] // 2)
    dbg.settle()
    time.sleep(0.3)
    res.check("releasing on a disabled item commits nothing", actions(dbg) == [],
              "a disabled row must not act")

    # --- 6. submenus: open on hover, placed to the right --------------
    dbg.send(f"gui key {ESC}")
    dbg.settle()
    dbg.click(*lay.centre("menu.title 2"))   # View
    dbg.settle()
    lay3 = wait_layout(dbg, content, lambda l: l.has("menu.item 0 0"))
    if not lay3.has("menu.item 0 0"):
        res.check("hovering a submenu parent opens the next level", False,
                  "View menu did not open")
        return
    go = lay3.rect("menu.item 0 0")          # "Go to"
    hover(dbg, qmp, go)
    lay4 = wait_layout(dbg, content, lambda l: l.popups() == [0, 1])
    res.check("hovering a submenu parent opens the next level",
              lay4.popups() == [0, 1], f"open levels: {lay4.popups()}")

    if lay4.popups() == [0, 1]:
        p0 = lay4.rect("menu.popup 0")
        p1 = lay4.rect("menu.popup 1")
        res.check("a submenu opens to the RIGHT of its parent, aligned to its row",
                  p1[0] >= p0[0] + p0[2] - 2 and abs(p1[1] - go[1]) <= 4,
                  f"parent {p0}, submenu {p1}, parent row y {go[1]}")

        # --- 7. commit on release, from the deepest level -------------
        actions(dbg)
        top = lay4.rect("menu.item 1 0")     # "Top of file"
        dbg.click(top[0] + top[2] // 2, top[1] + top[3] // 2)
        dbg.settle()
        got = wait_actions(dbg)
        res.check("releasing on a submenu item commits exactly that command",
                  got == [CMD_GOTO_TOP], f"actions: {got} (want [{CMD_GOTO_TOP}])")
        res.check("committing closes the whole chain",
                  wait_layout(dbg, content, lambda l: l.popups() == []).popups() == [],
                  "a committed menu must not stay open")

    # The real cursor is still parked on the submenu row from hover()
    # above, and it STAYS there -- that is the whole point of using it.
    # Left there it silently changes every later check: a menu opened
    # afterwards finds the pointer already inside it, so it can close or
    # open a submenu on its own. That is not hypothetical -- it turned
    # the dismiss check below into a vacuous pass ("the menu closed",
    # because it was never open) while its click-through partner went
    # red. Park it back on empty desktop before moving on.
    unpark(dbg, qmp)

    # --- 8. THE CANCEL PATH -------------------------------------------
    # Press an item, drag off the menu entirely, release. Nothing may
    # happen. This is the only check that can tell press-then-commit
    # from commit-on-press; see the positive control in the docstring.
    dbg.click(*lay.centre("menu.title 2"))
    dbg.settle()
    lay5 = wait_layout(dbg, content, lambda l: l.has("menu.item 0 2"))
    if lay5.has("menu.item 0 2"):
        sb = lay5.rect("menu.item 0 2")      # "Status bar"
        actions(dbg)
        tx, ty, tw, th = lay.rect("menu")
        dbg.drag(sb[0] + sb[2] // 2, sb[1] + sb[3] // 2,
                 tx + tw - 20, ty + th + 200)   # off the menu, into the text
        dbg.settle()
        time.sleep(0.4)
        res.check("a press dragged off a menu item commits nothing",
                  actions(dbg) == [], "press-then-commit-on-release")
        dbg.send(f"gui key {ESC}")
        dbg.settle()

    # --- 9. dismissing, and NOT reaching the text underneath ----------
    dbg.click(*lay.centre("menu.title 0"))
    dbg.settle()
    wait_layout(dbg, content, lambda l: l.popups() == [0])
    im3 = shot(qmp, tmp, "mb_before_dismiss.png")
    caret_pane = lay.rect("status.pane 1")     # "Ln n, Col n"
    before_caret = region(im3, caret_pane)

    # Somewhere in the text, well clear of the popup.
    tr = lay.rect("menu")
    away = (tr[0] + tr[2] - 60, tr[1] + tr[3] + 120)
    actions(dbg)
    dbg.click(*away)
    dbg.settle()
    dismissed = wait_layout(dbg, content, lambda l: l.popups() == [])
    im4 = shot(qmp, tmp, "mb_after_dismiss.png")
    res.check("a click outside dismisses the menu",
              dismissed.popups() == [], "the menu must close")
    res.check("the dismissing click does NOT reach the text underneath",
              region(im4, caret_pane) == before_caret,
              "the caret moved, so the click fell through to the editor")

    # --- 10. keyboard -------------------------------------------------
    dbg.send(f"gui key {F10}")
    dbg.settle()
    res.check("F10 opens the menu bar",
              wait_layout(dbg, content, lambda l: l.popups() == [0]).popups() == [0])

    # F10 opens File with its first row highlighted. Right on a row that
    # is NOT a submenu walks to the next TITLE -- the Windows/KDE
    # behaviour -- so File, Edit, View; and View's first row IS "Go to",
    # so the third Right opens it. Three Rights, ending two levels deep.
    #
    # Deliberately NOT routed through File > Recent files: it is disabled
    # until something has been saved, so the arrows correctly skip it,
    # Right walked to the next menu instead, and the Escs below then fell
    # through to Notepad's own Esc -- which QUITS. The first version of
    # this test killed the app here and every later check read stale
    # state. An arrow path has to be checked against what is enabled.
    for _ in range(3):
        dbg.send(f"gui key {RIGHT}")
    dbg.settle()
    depth_open = wait_layout(dbg, content, lambda l: l.popups() == [0, 1]).popups()

    dbg.send(f"gui key {ESC}")
    dbg.settle()
    after_one_esc = wait_layout(dbg, content, lambda l: l.popups() == [0]).popups()

    # Only if something is still open. An Esc with no menu open reaches
    # Notepad, which QUITS on it -- so a test that fires Escs blind turns
    # one failure into a cascade of unrelated ones.
    after_two = after_one_esc
    if after_one_esc:
        dbg.send(f"gui key {ESC}")
        dbg.settle()
        after_two = wait_layout(dbg, content, lambda l: l.popups() == []).popups()
    res.check("Esc closes ONE level at a time, not the whole chain",
              after_one_esc == [0] and after_two == [],
              f"opened {depth_open}, after one Esc {after_one_esc}, "
              f"after two {after_two}")

    # Notepad is still there. An explicit check because the failure it
    # guards against -- an Esc reaching the editor and quitting it --
    # makes every later check read stale geometry and fail for reasons
    # that have nothing to do with what they test.
    res.check("Notepad survived the keyboard navigation", npwin(dbg) is not None)
    if npwin(dbg) is None:
        return

    # --- 11. the status bar's panes -----------------------------------
    # Home FIRST, then compare against End. Sampling wherever the cursor
    # happens to be is not good enough: the "Go to > Top of file" commit
    # above had already put it at Ln 1, Col 1, so a Home that followed it
    # changed nothing and this check failed against a perfectly working
    # indicator. Establish the state you are measuring from.
    dbg.send("gui key 0x97")   # KEY_HOME
    dbg.settle()
    # A deliberate bounded wait, not a convertible one: the assertion is a
    # PIXEL diff of the status panes, and the cursor move has no layout
    # signal to poll (the pane rects don't change, only their rendered
    # text). Keep it a settle-plus-grace so the compositor has painted.
    time.sleep(0.4)
    im5 = shot(qmp, tmp, "mb_status_a.png")
    msg_before = region(im5, lay.rect("status.pane 0"))
    ind_before = region(im5, lay.rect("status.pane 1"))

    dbg.send("gui key 0x98")   # KEY_END -- moves the cursor, nothing else
    dbg.settle()
    time.sleep(0.4)
    im6 = shot(qmp, tmp, "mb_status_b.png")
    res.check("the status bar's indicator tracks the cursor while the message does not",
              region(im6, lay.rect("status.pane 1")) != ind_before
              and region(im6, lay.rect("status.pane 0")) == msg_before,
              "either half alone is satisfied by a bug: a dead indicator "
              "passes the second, a repainting-everything bar passes the first")

    # --- 12. a checkable item, round trip ------------------------------
    dbg.click(*lay.centre("menu.title 2"))
    dbg.settle()
    lay6 = wait_layout(dbg, content, lambda l: l.has("menu.item 0 2"))
    if lay6.has("menu.item 0 2"):
        sb = lay6.rect("menu.item 0 2")
        actions(dbg)
        dbg.click(sb[0] + sb[2] // 2, sb[1] + sb[3] // 2)
        dbg.settle()
        got = wait_actions(dbg)
        # has("status") reads the CURRENT frame, so it drops as soon as
        # the app redraws with the bar hidden -- an observable, not a sleep.
        gone = wait_layout(dbg, content, lambda l: not l.has("status"))
        res.check("View > Status bar hides the status bar",
                  got == [CMD_STATUSBAR] and not gone.has("status"),
                  f"actions {got}, statusbar still reported: {gone.has('statusbar')}")

        dbg.click(*lay.centre("menu.title 2"))
        dbg.settle()
        lay7 = wait_layout(dbg, content, lambda l: l.has("menu.item 0 2"))
        if lay7.has("menu.item 0 2"):
            sb = lay7.rect("menu.item 0 2")
            dbg.click(sb[0] + sb[2] // 2, sb[1] + sb[3] // 2)
            dbg.settle()
            wait_layout(dbg, content, lambda l: l.has("status"))
        back = layout(dbg, content)
        res.check("toggling it back restores the status bar exactly",
                  back.has("status") and back.r.get("status") == lay.r.get("status"),
                  f"was {lay.r.get('statusbar')}, now {back.r.get('statusbar')}")

    # --- 13. state is asked for: Recent fills in after a save ----------
    dbg.send("gui key 0x13")   # Ctrl-S -> Save As dialog
    dbg.settle()
    time.sleep(0.4)              # deliberate: the app's save dialog logs no
    typ(dbg, SAVE_NAME)         # layout line to poll for readiness
    dbg.send(f"gui key {ENTER}")
    dbg.settle()
    time.sleep(0.8)             # deliberate: wait out the blocking disk write
                                # (no clean completion signal exposed)

    dbg.click(*lay.centre("menu.title 0"))
    dbg.settle()
    lay8 = wait_layout(dbg, content, lambda l: l.has("menu.item 0 2"))
    if lay8.has("menu.item 0 2"):
        rec = lay8.rect("menu.item 0 2")
        hover(dbg, qmp, rec)
        lay9 = wait_layout(dbg, content, lambda l: l.popups() == [0, 1])
        res.check("Recent files is greyed until a save, then opens a real submenu",
                  lay9.popups() == [0, 1],
                  f"levels open over Recent after saving: {lay9.popups()} "
                  f"(it was disabled and unopenable before the save)")
        dbg.send(f"gui key {ESC}")
        dbg.send(f"gui key {ESC}")
        dbg.settle()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "menubar_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, args.tmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nmenubar_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
