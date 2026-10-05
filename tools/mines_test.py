#!/usr/bin/env python3
"""Drive Minesweeper (userland/gui/apps/mines.c) and assert on it.

WHAT THIS IS ACTUALLY FOR
-------------------------
Two things, and the second is the reason it exists at all.

1. The game's rules. Every one of those is asserted against the client's
   own `mines: state ...` log line, not against pixels: a screenshot
   cannot tell a flood fill that opened 51 cells from one that opened 3,
   and reading digits off a screen would be OCR nobody wants to
   maintain. The state line carries phase, flags, revealed and the
   losing cell, which is the whole rule set.

2. **A SECONDARY CLICK REACHES A RING-3 CLIENT.** That is a windowing
   protocol property, not a game one -- until this app, a right-click
   anywhere on a window (content included) was seized by the WM for its
   window menu, and no client had ever received one. The check that
   matters is the PAIR: a right-click on the BOARD must flag a cell and
   leave no context menu open, and a right-click on the TITLE BAR must
   still open the window menu. Either half alone passes under a
   half-broken split.

THE CHECKS THAT WOULD SURVIVE A BROKEN VERSION, AND WHY THEY DON'T
------------------------------------------------------------------
docs/gui-guidelines.md's rule is to ask what a broken version would
still pass. Three answers here:

  * "the first click didn't lose" passes ~88% of the time on a board
    with no first-click protection at all. So the check is `revealed >=
    9`, which the 3x3 safe NEIGHBOURHOOD guarantees (a clicked cell with
    no adjacent mines always floods at least its own 3x3) and which an
    implementation sparing only the clicked cell fails on most boards.
  * "the flag changed the state" passes on an app that never draws it
    (the Calculator-with-invisible-buttons failure). So the flag is also
    checked as PIXELS -- the flagged cell BEFORE against the same cell
    AFTER, with an untouched neighbour compared against itself the same
    way. Comparing two DIFFERENT cells is not a control and was the
    first version of this check: it passed because the cell it flagged
    happened to be revealed and a revealed cell differs from a covered
    one whether or not a flag ever drew.
  * "clicking dug a cell" passes on an app that acts on button-DOWN. So
    there is a press-drag-off check, the same one calculator_client_test
    carries, because commit-on-release is the rule most easily lost.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/mines_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TITLE = "Minesweeper"
SPAWN_PATH = "/bin/wm/apps/mines"
SPAWN_TIMEOUT_S = 15.0


class Layout:
    """Geometry as the CLIENT reports it -- never re-derived here.

    mines.c logs `mines: layout <what> ...` lines, content-relative.
    Inverting the app's own sizing formula in Python is the trap
    calculator_client_test.py documents at length: it stayed green while
    measuring something it no longer understood.
    """

    def __init__(self, content, lines):
        self.ox, self.oy = content["x"], content["y"]
        self.cw, self.ch = content["w"], content["h"]
        self.board = None
        self.face = None
        self.cell = 0
        self.titles = {}     # index -> rect
        self.items = {}      # (level, index) -> rect
        self.toast = None
        self.buttons = {}    # the Best Times dialog's, index -> rect
        self.minefield = []  # cell indices, once the first click placed them
        for line in lines:
            if "mines: layout " not in line:
                continue
            p = line.split("mines: layout ", 1)[1].split()
            if p[0] == "board" and len(p) >= 5:
                self.board = tuple(int(v) for v in p[1:5])
            elif p[0] == "face" and len(p) >= 5:
                self.face = tuple(int(v) for v in p[1:5])
            elif p[0] == "cell" and len(p) >= 2:
                self.cell = int(p[1])
            elif p[0] == "menu.title" and len(p) >= 6:
                self.titles[int(p[1])] = tuple(int(v) for v in p[2:6])
            elif p[0] == "menu.item" and len(p) >= 7:
                self.items[(int(p[1]), int(p[2]))] = tuple(int(v) for v in p[3:7])
            elif p[0] == "toast" and len(p) >= 5:
                self.toast = tuple(int(v) for v in p[1:5])
            elif p[0] == "best.button" and len(p) >= 6:
                self.buttons[int(p[1])] = tuple(int(v) for v in p[2:6])
            elif p[0] == "minefield" and len(p) >= 2:
                if int(p[1]) == 0:
                    self.minefield = []
                self.minefield += [int(v) for v in p[2:]]

    def complete(self):
        return self.board is not None and self.face is not None and self.cell > 0

    def cell_centre(self, col, row):
        bx, by, _, _ = self.board
        c = self.cell
        return (self.ox + bx + col * c + c // 2,
                self.oy + by + row * c + c // 2)

    def cell_box(self, col, row):
        """Screen crop box of one cell's INTERIOR, inside its border."""
        bx, by, _, _ = self.board
        c = self.cell
        x = self.ox + bx + col * c
        y = self.oy + by + row * c
        return (x + 2, y + 2, x + c - 2, y + c - 2)

    def face_centre(self):
        x, y, w, h = self.face
        return (self.ox + x + w // 2, self.oy + y + h // 2)

    def rect_centre(self, rect):
        x, y, w, h = rect
        return (self.ox + x + w // 2, self.oy + y + h // 2)


Result = Results


def parse_state(lines):
    """The last `mines: state ...` line, as a dict of ints (plus `what`)."""
    out = None
    for line in lines:
        if "mines: state " not in line:
            continue
        parts = line.split("mines: state ", 1)[1].split()
        d = {"what": parts[0]}
        for kv in parts[1:]:
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    d[k] = int(v)
                except ValueError:
                    pass
        out = d
    return out


def state_after(dbg, action, tries=20):
    """Run `action`, then return the state line it produced.

    Polls for the line rather than sleeping a fixed time: the client has
    to be scheduled, draw and log before it appears, and how long that
    takes varies with load (CLAUDE.md -- wait on the OBSERVABLE).
    """
    dbg.logs("mines:", clear=True)
    action()
    for _ in range(tries):
        st = parse_state(dbg.logs("mines:", clear=True))
        if st:
            return st
        time.sleep(0.2)
    return None


def spawn(dbg):
    """Start the game; (window, log lines, Layout), any of them None."""
    dbg.send(f"gui spawn {SPAWN_PATH}")
    deadline = time.time() + SPAWN_TIMEOUT_S
    win, lines, lay = None, [], None
    while time.time() < deadline:
        lines += dbg.logs("mines:", clear=True)
        win = dbg.window(TITLE)
        if win:
            lay = Layout(win["content"], lines)
            if lay.complete():
                break
        time.sleep(0.3)
    return win, lines, lay


def close(dbg, win):
    """Close it from its title bar; True once the window is gone."""
    dbg.send(f"gui click {win['x'] + win['w'] - 14} {win['y'] + 14}")
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    while time.time() < deadline:
        if dbg.window(TITLE) is None:
            return True
        time.sleep(0.2)
    return False


def luminance(rgb):
    r, g, b = rgb[:3]
    return (299 * r + 587 * g + 114 * b) // 1000


def play_to_win(dbg, qmp, tmp, res, lay):
    """Win a Beginner game, check what a win shows, and that its best
    time outlives the process. Returns the best time it set, or None.

    THE CLIENT SAYS WHERE ITS MINES ARE (a layout-log line written when
    the first click places them), so this WINS rather than hoping to: a
    test that guesses would reach the win path one run in hundreds.
    """
    acc = []
    dbg.logs("mines:", clear=True)
    dbg.send("gui click %d %d" % lay.cell_centre(4, 4))
    field = []
    for _ in range(25):
        acc += dbg.logs("mines:", clear=True)
        field = Layout({"x": lay.ox, "y": lay.oy, "w": lay.cw, "h": lay.ch}, acc).minefield
        if len(field) == 10:
            break
        time.sleep(0.2)
    res.check("the client reports where its mines are", len(field) == 10, f"minefield={field}")
    if len(field) != 10:
        return None

    # A click on an already-opened cell is harmless: a 0 does nothing, and
    # a number with no flags round it is not a satisfied chord.
    for i in range(81):
        if i not in field:
            dbg.send("gui click %d %d" % lay.cell_centre(i % 9, i // 9))
    won = None
    for _ in range(40):
        acc += dbg.logs("mines:", clear=True)
        st = parse_state(acc)
        if st and st["phase"] == 2:
            won = st
            break
        time.sleep(0.2)
    res.check("opening every safe cell wins, flags every mine and sets a best time",
              won is not None and won["flags"] == 10 and won["revealed"] == 71
              and won["best"] > 0, f"state={won}")
    if not won:
        return None

    # THE NOTE IS DRAWN, not merely laid out: a settled frame (so the
    # rise and the confetti are over) has the pill's dark ink where the
    # client says the note is, and the board's first cell -- a flag on
    # a covered tile, nowhere near the note -- stays light.
    note = Layout({"x": lay.ox, "y": lay.oy, "w": lay.cw, "h": lay.ch}, acc).toast
    res.check("the win puts up its 'Solved in' note", note is not None, f"toast={note}")
    if note:
        shot = os.path.abspath(os.path.join(tmp, "mines_won.png"))
        px = qmp.stable_pixels(shot, None)
        tx, ty, tw, th = note
        inside = px.getpixel((lay.ox + tx + th // 2, lay.oy + ty + th // 2))
        bx, by, _, _ = lay.board
        control = px.getpixel((lay.ox + bx + 3, lay.oy + by + lay.cell // 2))
        res.check("...and draws it", luminance(inside) < 90 and luminance(control) > 150,
                  f"note {inside}, first cell {control}")
    return won["best"]


def run(dbg, qmp, tmp, res):
    win, lines, lay = spawn(dbg)

    res.check("Minesweeper runs as a ring-3 process with its own window",
              win is not None, f"no window titled {TITLE!r} in {SPAWN_TIMEOUT_S}s")
    if not win:
        return
    lay = lay or Layout(win["content"], lines)
    res.check("it reports its own board geometry", lay.complete(),
              f"board={lay.board} face={lay.face} cell={lay.cell}")
    if not lay.complete():
        return

    start = parse_state(lines)
    res.check("a fresh board is Beginner, covered, with no mines placed yet",
              start is not None and start["phase"] == 0 and start["cols"] == 9
              and start["rows"] == 9 and start["mines"] == 10
              and start["revealed"] == 0 and start["flags"] == 0,
              f"state={start}")

    # --- the board is DRAWN, not merely laid out ----------------------
    #
    # The Calculator-with-invisible-buttons failure, asked here: a
    # covered cell's face must differ from the window background behind
    # the board. Control point included, per docs/gui-guidelines.md.
    from PIL import Image
    shot = os.path.abspath(os.path.join(tmp, "mines_board.png"))
    bx, by, bw, bh = lay.board
    # SETTLED, not a raw grab. A capture landing mid-paint reads the
    # backdrop where the board is about to be, which is precisely what
    # this check calls "not painted" -- it failed once in a full
    # gui_regress run and passed 3 of 3 alone, the signature of the
    # timing this repo's stable_pixels() exists for. Every other pixel
    # check in this file already goes through it. The box is the board
    # plus a margin, so the control point below comes out of the SAME
    # settled frame; the whole screen would never settle, because the
    # taskbar clock ticks.
    qmp.stable_pixels(shot, (lay.ox + bx - 8, lay.oy + by,
                             lay.ox + bx + bw, lay.oy + by + bh))
    with Image.open(shot) as im:
        px = im.convert("RGB")
        cover = px.getpixel((lay.ox + bx + 4, lay.oy + by + 4))
        # Just outside the board's left edge: window background.
        backdrop = px.getpixel((lay.ox + bx - 4, lay.oy + by + 4))
    res.check("covered cells are actually painted, not just clickable",
              cover != backdrop,
              f"cell face {cover} is indistinguishable from the backdrop {backdrop}")

    # --- the clock does not run before the first click ----------------
    time.sleep(2.5)
    idle = state_after(dbg, lambda: dbg.send("gui move %d %d" % lay.cell_centre(0, 0)))
    # A move produces no state line; read the last one we have instead.
    res.check("the clock does not start until the first click",
              start["elapsed"] == 0 and (idle is None or idle["elapsed"] == 0),
              f"elapsed={idle}")

    # --- THE FIRST CLICK IS SAFE, AND IT OPENS AN AREA ----------------
    #
    # >= 9 revealed is what the 3x3 safe neighbourhood guarantees. Run
    # over three fresh boards: one lucky board proves nothing.
    #
    # A WIN COUNTS AS PASSING. `phase` 1 is in progress and 2 is over;
    # requiring 1 fails a board whose first click cascaded to every safe
    # cell, which is a legal and occasionally-real Minesweeper outcome --
    # measured once in four full-suite runs, board 9x9/10, all 71 safe
    # cells opened at once. What this check is about is `boom` and the
    # opened area, and a win satisfies both maximally. Requiring the
    # game to still be running turned the luckiest possible board into a
    # failure, and took the clock check below down with it, because a
    # finished game's clock correctly stops.
    ok_first, detail = True, ""
    won_instantly = False
    for i in range(3):
        st = state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.cell_centre(4, 4)))
        if st is None or st["phase"] not in (1, 2) or st["boom"] != -1 or st["revealed"] < 9:
            ok_first = False
            detail = f"board {i}: {st}"
            break
        if st["phase"] == 2:
            won_instantly = True
        if i < 2:
            state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.face_centre()))
    res.check("the first click never hits a mine and always opens an area",
              ok_first, detail)

    # --- the clock runs once a game is under way ----------------------
    #
    # Asserted as "it ADVANCES", not as "it reached N after N seconds of
    # wall clock". The guest's own timer runs at whatever rate TCG can
    # emulate -- measured here at well under half real time under load --
    # so a check comparing guest seconds against host seconds is a flake
    # dressed up as a measurement. A right-click on an already-open cell
    # is the sampling poke: it logs the state and changes nothing.
    #
    # A GAME THAT ENDED CANNOT BE TIMED, so start a fresh one when the
    # loop above finished on a won board -- otherwise this asserts that
    # a stopped clock advances, which is the app behaving correctly.
    if won_instantly:
        state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.face_centre()))
        state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.cell_centre(4, 4)))
    sample = lay.cell_centre(4, 4)
    t1 = state_after(dbg, lambda: dbg.rclick(*sample))
    advanced = False
    deadline = time.time() + 25
    while t1 is not None and time.time() < deadline:
        time.sleep(1.0)
        t2 = state_after(dbg, lambda: dbg.rclick(*sample))
        if t2 is not None and t2["elapsed"] > t1["elapsed"]:
            advanced = True
            break
    res.check("the clock runs while a game is in progress", advanced,
              f"elapsed stuck at {t1 and t1['elapsed']} for 25s of host time")

    # --- A RIGHT-CLICK REACHES THE CLIENT -----------------------------
    #
    # The protocol check, and the reason this file exists.
    #
    # Done on a FRESH board, deliberately: a flag can only be placed on a
    # COVERED cell, and after a first click the flood may well have
    # opened the corner. Asserting on a cell whose state you do not know
    # is how this check first "failed" -- and worse, how its pixel half
    # first PASSED for the wrong reason (it compared a revealed cell
    # against a covered one, which differ whether or not a flag drew).
    before = state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.face_centre()))
    res.check("a fresh board for the pointer checks",
              before is not None and before["phase"] == 0 and before["flags"] == 0,
              f"state={before}")
    if before is None:
        # Nothing below can mean anything without a baseline, and every
        # one of them would crash on it. A tool that dies mid-run reports
        # as an ERROR rather than as the failures it actually found,
        # which is strictly less information -- so bail with the reason.
        res.check("...so the remaining pointer checks could not run", False,
                  "the app stopped answering -- is a WM popup swallowing the clicks?")
        return

    flag_box = lay.cell_box(0, 0)
    ctrl_box = lay.cell_box(0, 2)
    flag_before = qmp.stable_pixels(os.path.abspath(os.path.join(tmp, "mines_f0.png")), flag_box)
    ctrl_before = qmp.stable_pixels(os.path.abspath(os.path.join(tmp, "mines_c0.png")), ctrl_box)

    flagged = state_after(dbg, lambda: dbg.rclick(*lay.cell_centre(0, 0)))
    res.check("a right-click on the board reaches the client and flags a cell",
              flagged is not None and flagged["flags"] == 1,
              f"flags {before['flags']} -> {flagged and flagged['flags']}")
    ctx = dbg.send("gui ctxmenu")
    res.check("...and the WM did NOT swallow it into a window menu",
              "closed" in ctx, ctx.strip())
    if flagged is None:
        res.check("...so the remaining flag checks could not run", False,
                  "no state line after the right-click")
        return

    # The flag is DRAWN. The control is the SAME cells before and after:
    # the flagged one must change, an untouched one must not.
    flag_after = qmp.stable_pixels(os.path.abspath(os.path.join(tmp, "mines_f1.png")), flag_box)
    ctrl_after = qmp.stable_pixels(os.path.abspath(os.path.join(tmp, "mines_c1.png")), ctrl_box)
    res.check("the flag is drawn, not merely recorded",
              flag_after != flag_before and ctrl_after == ctrl_before,
              "flagged cell changed=%s, untouched neighbour changed=%s"
              % (flag_after != flag_before, ctrl_after != ctrl_before))

    # --- a flagged cell is protected from a dig -----------------------
    protected = state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.cell_centre(0, 0)))
    res.check("a left-click on a flagged cell does nothing",
              protected is not None and protected["revealed"] == 0
              and protected["phase"] == 0,
              f"state={protected}")

    # --- right-click again unflags ------------------------------------
    unflagged = state_after(dbg, lambda: dbg.rclick(*lay.cell_centre(0, 0)))
    res.check("a second right-click removes the flag",
              unflagged is not None and unflagged["flags"] == 0,
              f"flags={unflagged and unflagged['flags']}")

    # --- press, drag OFF, release must NOT dig ------------------------
    #
    # The rule every control in this GUI follows. An app acting on
    # button-DOWN passes every check above and fails this one.
    bx, by = lay.cell_centre(0, 0)
    outside = lay.ox + lay.cw + 40
    dragged = state_after(dbg, lambda: dbg.drag(bx, by, outside, by), tries=8)
    res.check("a press dragged off its cell does NOT dig it",
              dragged is None or (dragged["revealed"] == 0 and dragged["phase"] == 0),
              f"state={dragged}")

    # --- the smiley starts a new game ---------------------------------
    state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.cell_centre(4, 4)))
    fresh = state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.face_centre()))
    res.check("clicking the face starts a new game",
              fresh is not None and fresh["phase"] == 0 and fresh["revealed"] == 0
              and fresh["flags"] == 0 and fresh["elapsed"] == 0,
              f"state={fresh}")

    # --- a won game, and the best time it leaves behind ---------------
    best = play_to_win(dbg, qmp, tmp, res, lay)
    if best:
        # A ROUND TRIP THROUGH THE DISK: a new process reads it back.
        res.check("the game closes", close(dbg, win))
        win, lines, lay = spawn(dbg)
        reopened = parse_state(lines)
        res.check("a new Minesweeper remembers the best time",
                  win is not None and lay.complete() and reopened is not None
                  and reopened["best"] == best, f"state={reopened} best={best}")
        if not win or not lay.complete():
            return
        # Best Times... resets it, through the dialog's Reset button.
        dbg.logs("mines:", clear=True)
        dbg.send("gui click %d %d" % lay.rect_centre(lay.titles[0]))
        dbg.settle()
        time.sleep(0.4)
        menu = Layout(win["content"], dbg.logs("mines:", clear=True))
        row = menu.items.get((0, 6))
        res.check("the Game menu has Best Times...", row is not None, f"rows={sorted(menu.items)}")
        if row:
            dbg.send("gui click %d %d" % lay.rect_centre(row))
            dbg.settle()
            time.sleep(0.4)
            dlg = Layout(win["content"], dbg.logs("mines:", clear=True))
            reset = dlg.buttons.get(0)
            res.check("Best Times opens a dialog with its buttons", reset is not None,
                      f"buttons={dlg.buttons}")
            if reset:
                st = state_after(dbg, lambda: dbg.send("gui click %d %d" % lay.rect_centre(reset)))
                res.check("Reset clears the best time", st is not None
                          and st["what"] == "best-reset" and st["best"] == 0, f"state={st}")

    # --- the title bar still opens the WINDOW MENU --------------------
    #
    # The other half of the split. Without this, "the client gets every
    # right-click" would pass and the window menu would be unreachable.
    dbg.rclick(win["x"] + win["w"] // 2, win["y"] + 6)
    dbg.settle()
    ctx = dbg.send("gui ctxmenu")
    res.check("a right-click on the TITLE BAR still opens the window menu",
              "open" in ctx and "Close" in ctx, ctx.strip())
    dbg.send("gui click %d %d" % (win["x"] + win["w"] // 2, win["y"] - 30))  # dismiss
    dbg.settle()

    # --- difficulty resizes the board AND the window ------------------
    dbg.logs("mines:", clear=True)
    dbg.send("gui click %d %d" % lay.rect_centre(lay.titles[0]))
    dbg.settle()
    time.sleep(0.4)
    menu = Layout(win["content"], dbg.logs("mines:", clear=True))
    # Row 4 of the Game menu: New Game, ---, Beginner, Intermediate, Expert
    expert = menu.items.get((0, 4))
    res.check("the Game menu opens and reports its rows", expert is not None,
              f"rows={sorted(menu.items)}")
    if expert:
        # Popup rects are content-relative to the same origin.
        st = state_after(dbg, lambda: dbg.send(
            "gui click %d %d" % (lay.ox + expert[0] + expert[2] // 2,
                                 lay.oy + expert[1] + expert[3] // 2)))
        res.check("Expert switches the board to 30x16 with 99 mines",
                  st is not None and st["cols"] == 30 and st["rows"] == 16
                  and st["mines"] == 99, f"state={st}")
        grown = None
        for _ in range(20):
            grown = dbg.window(TITLE)
            if grown and grown["content"]["w"] > lay.cw:
                break
            time.sleep(0.2)
        res.check("...and the window grows with it",
                  grown is not None and grown["content"]["w"] > lay.cw,
                  f"content width {lay.cw} -> {grown and grown['content']['w']}")
        win = grown or win

    # --- it closes politely -------------------------------------------
    res.check("it closes on request", close(dbg, win))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "mines_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, res)
    finally:
        dbg.close()

    print(f"\nmines_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
