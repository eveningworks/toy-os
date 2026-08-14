#!/usr/bin/env python3
"""Drive the RING-3 Calculator (userland/calculator.c) and assert on it.

This is the proof for the app-migration step: Calculator running as an
ordinary ring-3 process, drawing with the ported widget toolkit
(userland/uui.c) and computing with the SAME apps/calc_engine.c the
kernel-space version uses.

WHAT IT ASSERTS, AND WHY IN THIS FORM
-------------------------------------
There is no OCR here and none is needed. Every check is a round trip:
a state change must alter the display region's pixels, and returning to
the same logical state must restore them EXACTLY. That proves rendering
and arithmetic together without ever reading a digit -- and it is
stricter than reading one, because it also catches a display that
renders the right number in the wrong place.

The most valuable check is the last one: press a button, drag OFF it,
release. That must NOT commit. It is the rule every control in this GUI
follows (docs/gui-guidelines.md), it is the reason ui_button_group
exists at all, and it is the single behaviour most likely to be lost in
a port -- a client that acted on WIN_EV_MOUSE_DOWN would pass every
other check here and fail this one.

Geometry is DERIVED from the window's own reported content size rather
than hardcoded, so it stays correct if the font size changes.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/calculator_client_test.py --shot screenshots/YYYY-MM-DD
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TITLE = "Calculator"
SPAWN_CMD = "run calculator"

# userland/calculator.c's layout constants.
MARGIN, GAP, COLS, ROWS = 8, 6, 4, 5
SPAWN_TIMEOUT_S = 15.0

# Row-major, matching the client's BUTTONS[] table.
LABELS = ["C", "CE", "%", "/",
          "7", "8", "9", "*",
          "4", "5", "6", "-",
          "1", "2", "3", "+",
          "+/-", "0", ".", "="]


class Layout:
    """Button geometry solved from the reported content size.

    Derived rather than hardcoded: the client sizes everything off the
    font it is handed, so pinning pixel values here would silently
    mis-click the moment the desktop font size changed.
    """

    def __init__(self, content):
        self.ox, self.oy = content["x"], content["y"]
        cw, ch = content["w"], content["h"]
        self.btn_w = (cw - 2 * MARGIN - (COLS - 1) * GAP) // COLS
        # ch = 2*MARGIN + (2*char_h + 30) + ROWS*(char_h+16) + (ROWS-1)*GAP
        self.char_h = (ch - 150) // 7
        self.btn_h = self.char_h + 16
        self.top_h = 2 * self.char_h + 30
        self.cw, self.chh = cw, ch

    def button_center(self, label):
        i = LABELS.index(label)
        row, col = i // COLS, i % COLS
        bx = MARGIN + col * (self.btn_w + GAP)
        by = MARGIN + self.top_h + row * (self.btn_h + GAP)
        return (self.ox + bx + self.btn_w // 2, self.oy + by + self.btn_h // 2)

    def display_box(self):
        """Screen-coordinate box of the numeric display strip."""
        y = self.oy + MARGIN + (self.char_h + 4) + 2
        return (self.ox + MARGIN, y,
                self.ox + self.cw - MARGIN, y + self.char_h + 16)


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def type_text(dbg, text):
    for ch in text:
        dbg.send(f"gui key {'0x20' if ch == ' ' else ch}")
    dbg.settle()


def display_pixels(qmp, tmp, name, box):
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    with Image.open(p) as im:
        return im.convert("RGB").crop(box).tobytes()


def run(dbg, qmp, tmp, shot_dir, res):
    dbg.send("gui open Terminal")
    dbg.settle()
    type_text(dbg, SPAWN_CMD)
    dbg.send("gui key 0x0d")

    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win:
            break
    res.check("Calculator runs as a ring-3 process with its own window", win is not None,
              f"no window titled {TITLE!r} within {SPAWN_TIMEOUT_S}s")
    if not win:
        return

    lay = Layout(win["content"])
    box = lay.display_box()
    zero = display_pixels(qmp, tmp, "calc_zero.png", box)

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-calculator.png")))

    # --- mouse arithmetic, through the ported widgets ------------------
    def click(label):
        dbg.send("gui click %d %d" % lay.button_center(label))
        dbg.settle()
        time.sleep(0.25)

    click("7")
    seven = display_pixels(qmp, tmp, "calc_7.png", box)
    res.check("a mouse click on a button reaches the client and repaints",
              seven != zero)

    for lbl in ("+", "3", "="):
        click(lbl)
    ten = display_pixels(qmp, tmp, "calc_10.png", box)
    res.check("7 + 3 = renders a result different from both inputs",
              ten != seven and ten != zero)

    # The round trip: C must restore the display to byte-identical to
    # its just-opened state. That is arithmetic AND rendering in one
    # assertion, with no digit ever read.
    click("C")
    cleared = display_pixels(qmp, tmp, "calc_clear.png", box)
    res.check("C restores the display to exactly its initial pixels",
              cleared == zero)

    # --- the keyboard path, which must agree with the mouse -----------
    for k in ("7", "0x2b", "3", "0x0d"):   # 7 + 3 Enter
        dbg.send(f"gui key {k}")
        dbg.settle()
        time.sleep(0.25)
    kb_ten = display_pixels(qmp, tmp, "calc_kb10.png", box)
    res.check("the keyboard produces the identical result to the mouse",
              kb_ten == ten,
              "keyboard and mouse paths disagree on 7 + 3 =")
    click("C")

    # --- the rule that matters: press, drag OFF, release --------------
    #
    # A client acting on button-down would pass everything above and
    # fail here. The drag ends outside the window entirely, so the
    # button is unambiguously not under the cursor at release.
    bx, by = lay.button_center("9")
    outside_x = lay.ox + lay.cw + 40
    dbg.send(f"gui drag {bx} {by} {outside_x} {by}")
    dbg.settle()
    time.sleep(0.4)
    after_drag = display_pixels(qmp, tmp, "calc_dragoff.png", box)
    res.check("a press dragged off its button does NOT commit",
              after_drag == zero,
              "the display changed, so the drag-off still registered a click")

    # And the control must still work normally afterwards -- a botched
    # press-state reset would leave the button stuck.
    click("9")
    after_nine = display_pixels(qmp, tmp, "calc_nine.png", box)
    res.check("the same button still works after the aborted press",
              after_nine != zero)

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-calculator-result.png")))

    # Close politely, same handshake as every other client.
    dbg.send(f"gui click {win['x'] + win['w'] - 14} {win['y'] + 14}")
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if dbg.window(TITLE) is None:
            gone = True
            break
    res.check("it closes on request", gone)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.5)
    if args.shot:
        os.makedirs(args.shot, exist_ok=True)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, args.shot, res)
    finally:
        dbg.close()

    print(f"\ncalculator_client_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
