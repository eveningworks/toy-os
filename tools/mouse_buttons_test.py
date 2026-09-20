#!/usr/bin/env python3
"""tools/mouse_buttons_test.py -- a pointer button is an EDGE, not a level.

WHAT THIS COVERS
----------------
Five buttons reach a ring-3 client, and a SHORT press reaches it too.
The second half is the one that was broken: every stage between the
device and the client kept the button mask as a LEVEL and sampled it.
`hid_service_one()` drains every queued USB report in one pass,
`win_input_poll()` compares the mask against the last one it saw and
runs from `scheduler_idle()`, and `wm_rawin_pump()` assigned the newest
mask over whatever was there. So a press and its release landing
between two passes cancelled out and the click never happened -- which
is why the File Manager's thumb-button Back worked about one time in
twenty, and why holding the button down made it work.

THE ASSERTION THAT MATTERS
--------------------------
Check 3. `QMPSession.tap()` sends the press and the release in ONE
`input-send-event`, so both edges are in the guest before it can look.
Anything that samples a level sees the mask at rest and reports
nothing. Check 2 -- the same button HELD -- is what passes on the broken
build, and the pair is what makes the difference legible: 2 green with
3 red is exactly the shape of a sampled level.

Checks 4 and 5 are the user-visible half: a tapped thumb button
navigates the File Manager, which is the app that gives them a meaning
(Explorer's and Dolphin's, and every browser's).

WHY IT READS UI DEMO'S LOG
--------------------------
`userland/gui/demos/uidemo.c` reports the live mask by name on every
press and release -- it is this tree's `xev`, and the only place
buttons 3, 4 and 5 are visible at all, since the toolkit routes only
the primary button to widgets. Reading the log rather than pixels is
deliberate: this is a question about input reaching a client, not about
rendering.

POSITIVE CONTROL
----------------
Run with --control to have the tool tell you what to break. The cheap
one: in `kernel/proc/win_input.c`, replace the drain loop with the
sample it replaced --

    if (x != g_last_x || y != g_last_y || buttons != g_last_buttons)
        push(WIN_EV_RAW_MOUSE, x, y, buttons);

MEASURED, not predicted: 2 fail and 5 pass -- checks 3 and 4 go red
while 1, 2, 5a and 5b stay green. A second control, for the compositor
half: restore `g_buttons = (uint8_t)ev.mods;` in `wm_rawin_pump()` and
delete the queue -- same two checks, same colours, which is the point.
Either layer alone loses the tap.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui
from qmp_test import QMPSession
import port_guard  # noqa: E402

UIDEMO_TITLE = "UI Demo"
FILES_TITLE = "File Manager"
OPEN_TIMEOUT_S = 20.0



class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def settle_input(dbg, seconds=0.8):
    """Wait for a LOG, not for a frame -- so there is nothing to stabilise.
    dbg.settle() is what pumps the console; the sleep is what gives the
    WM and the client a few frames to run in between."""
    dbg.settle()
    time.sleep(seconds)
    dbg.settle()


def masks(dbg):
    """UI Demo's button lines since the last call, as (what, mask, names)."""
    out = []
    for line in dbg.logs("uidemo: "):
        parts = line.split()
        for i, p in enumerate(parts):
            if p in ("press", "release") and i + 1 < len(parts):
                try:
                    out.append((p, int(parts[i + 1], 16), " ".join(parts[i + 2:])))
                except ValueError:
                    pass
                break
    return out


def pane_dir(dbg, pane=0):
    """Where the File Manager's pane is, from the app's own layout report
    (`files: layout dir <pane> <path>`, userland/fm/fm_view.c) -- the same
    observable filemanager_test.py reads, rather than a guess from pixels."""
    seen = None
    for line in dbg.logs("files: layout dir"):
        parts = line.split()
        for i, p in enumerate(parts):
            if p == "dir" and i + 2 < len(parts) and parts[i + 1] == str(pane):
                seen = parts[i + 2]
                break
    return seen


def pane_geom(dbg):
    """(x, rowy, rowh) for pane 0, from the app's own layout report.
    Row 0's y is reported rather than derived, because deriving it as
    "pane top + n * row height" lands on the row above -- the header is
    in the way, and a neighbouring row is a plausible thing to have
    clicked (fm_view.c says so at the line that logs it)."""
    x = rowy = rowh = None
    for line in dbg.logs("files: layout", clear=False):
        p = line.split()
        for i, w in enumerate(p):
            if w == "pane" and i + 5 < len(p) and p[i + 1] == "0":
                x = int(p[i + 2])
            elif w == "rowy" and i + 2 < len(p) and p[i + 1] == "0":
                rowy = int(p[i + 2])
            elif w == "rowh" and i + 1 < len(p):
                rowh = int(p[i + 1])
    return x, rowy, rowh


def open_app(dbg, title, timeout=OPEN_TIMEOUT_S):
    """`gui open` by name -- no pixels, so a moved menu cannot break it."""
    dbg.open_app(title)
    deadline = time.time() + timeout
    while time.time() < deadline:
        win = dbg.window(title)
        if win:
            return win
        time.sleep(0.3)
    return None


def over(qmp, win):
    """Park the pointer well inside a window's content, away from chrome."""
    qmp.goto(win["x"] + win["w"] // 2, win["y"] + win["h"] - 40)


def run(dbg, qmp, res):
    win = open_app(dbg, UIDEMO_TITLE)
    res.check("1. UI Demo opens and reports a window", win is not None,
              f"no window titled {UIDEMO_TITLE!r} within {OPEN_TIMEOUT_S}s")
    if not win:
        return

    # Focus with an ordinary click, then discard what it logged: the
    # checks below must read their own events and not this one's.
    over(qmp, win)
    settle_input(dbg)
    qmp.click()
    settle_input(dbg)
    masks(dbg)

    # --- 2. a HELD thumb button, which is what passes on the old build --
    qmp.mouse_down("side")
    settle_input(dbg)
    held = masks(dbg)
    res.check("2. a held thumb button reports a press, by name",
              any(w == "press" and m & 0x08 and "side" in n for w, m, n in held),
              f"expected a press with SIDE (0x08) set, got {held}")
    qmp.mouse_up("side")
    settle_input(dbg)
    masks(dbg)

    # --- 3. THE ONE THAT MATTERS: press and release in ONE batch --------
    #
    # Both edges arrive before the guest can poll. A level-sampling path
    # reads the mask back at rest and reports nothing at all.
    for name, button, bit in (("side", "side", 0x08), ("extra", "extra", 0x10)):
        qmp.tap(button)
        settle_input(dbg)
        t = masks(dbg)
        res.check(f"3. a TAPPED {name} button survives one polling pass",
                  any(w == "press" and m & bit for w, m, _ in t),
                  f"the press was lost between the device and the client: {t}")
        res.check(f"3. ...and its release arrives too ({name})",
                  any(w == "release" for w, _, _ in t),
                  f"a button the client believes is still held: {t}")

    # A tapped PRIMARY button, which nothing here ever lost -- so a build
    # where check 3 is red and this is green has a path that works and a
    # sampling stage that loses the short ones, rather than a dead mouse.
    qmp.tap("left")
    settle_input(dbg)
    t = masks(dbg)
    res.check("3. a tapped primary button survives it as well",
              any(w == "press" and m & 0x01 for w, m, _ in t), str(t))

    # --- 4. what it is FOR: the thumb buttons navigate the File Manager -
    files = open_app(dbg, FILES_TITLE)
    res.check("5a. the File Manager opens", files is not None)
    if not files:
        return

    settle_input(dbg)
    start = pane_dir(dbg)

    # **DESCEND WITH THE MOUSE, NOT THE KEYBOARD.** This used to press
    # Down then Enter, and that is the one path that cannot fail: the
    # app records history around its own key handler. A double click
    # activates a row from INSIDE uui_fileview, reaching no fm_goto*()
    # at all -- so with history recorded only at those call sites, Back
    # was dead for everyone using a mouse and this check stayed green.
    # Ask what a broken version would still pass (CLAUDE.md).
    px, rowy, rowh = pane_geom(dbg)
    res.check("5c. the File Manager reports its pane and row geometry",
              None not in (px, rowy, rowh),
              f"pane x={px} rowy={rowy} rowh={rowh}")
    if None in (px, rowy, rowh):
        return
    # Row 0, a little in from the left edge so the click is on the name
    # column rather than on a resize gutter. Twice: the fileview counts
    # a double by the ROW, not by a clock.
    #
    # CONTENT-RELATIVE PLUS THE WINDOW'S ORIGIN: the app reports layout
    # in its own surface's coordinates and `gui click` takes SCREEN
    # ones, so clicking the reported numbers directly lands outside the
    # window -- which reads exactly like a dead control.
    ox, oy = files["content"]["x"], files["content"]["y"]
    rx, ry = ox + px + 40, oy + rowy + rowh // 2
    # BACK TO BACK, with no settle between them. dbg.click() settles,
    # which puts about a second between the two halves -- and a pane
    # RELOAD clears uui_fileview's last_click_row, so on a machine that
    # is writing to disk (this one logs continuously) the second click
    # is a fresh single click and nothing descends. Measured: settled,
    # the pane stayed at '/'; back to back it reaches /bin.
    dbg.send(f"gui click {rx} {ry}")
    dbg.send(f"gui click {rx} {ry}")
    settle_input(dbg, 2.0)
    inner = pane_dir(dbg)
    res.check("5b. the pane descends, so there is history to go back through",
              inner is not None and inner != start,
              f"pane 0 stayed at {start!r}")
    if inner is None or inner == start:
        return

    over(qmp, files)
    settle_input(dbg)
    qmp.tap("side")
    settle_input(dbg, 1.5)
    now = pane_dir(dbg)
    res.check("4. a TAPPED thumb button navigates the File Manager back",
              now == start,
              f"pane 0 is at {now!r}, not back at {start!r} -- either the "
              "tap never reached the client, or the descent above recorded "
              "no history for it to go back through (fm_history.c)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--control", action="store_true",
                    help="print the positive control and exit")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "mouse_buttons_test")

    if args.control:
        print(__doc__.split("POSITIVE CONTROL")[1].strip())
        return 0

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nmouse_buttons_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
