#!/usr/bin/env python3
"""Drive the RING-3 Calculator (userland/gui/calculator.c) and assert on it.

This is the proof for the app-migration step: Calculator running as an
ordinary ring-3 process, drawing with the ported widget toolkit
(userland/ui/uui.c) and computing with the SAME apps/calc_engine.c the
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
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TITLE = "Calculator"
SPAWN_PATH = "/bin/wm/apps/calculator"   # spawned directly -- see run()

SPAWN_TIMEOUT_S = 15.0

# Row-major, matching the client's BUTTONS[] table.
LABELS = ["C", "CE", "%", "/",
          "7", "8", "9", "*",
          "4", "5", "6", "-",
          "1", "2", "3", "+",
          "+/-", "0", ".", "="]


class Layout:
    """Button geometry as the CLIENT reports it, not as Python guesses.

    Calculator logs one `calculator: layout <what> [name] x y w h` line
    per widget at startup (content-relative), the same grammar UI Demo
    and Shapes use. This class just parses them.

    It used to solve the geometry from the reported content size
    instead, inverting the app's own sizing formula -- including a
    literal `char_h = (ch - 150) // 7`. That is the trap both other GUI
    test tools document, and it sprang: when Calculator moved to
    uui_layout the inversion produced a char_h of 18 against a real 17
    and buttons 44px wide against a real 40, so every click landed
    several pixels off centre. They still landed INSIDE the buttons, so
    this suite stayed green while measuring something it no longer
    understood -- which is worse than a red test, because nothing asks
    you to look.
    """

    def __init__(self, content, lines):
        self.ox, self.oy = content["x"], content["y"]
        self.cw, self.chh = content["w"], content["h"]
        self.buttons = {}   # label -> (x, y, w, h), content-relative
        self.display = None
        for l in lines:
            if "calculator: layout " not in l:
                continue
            parts = l.split("calculator: layout ", 1)[1].split()
            if parts[0] == "btn" and len(parts) >= 6:
                self.buttons[parts[1]] = tuple(int(v) for v in parts[2:6])
            elif parts[0] == "display" and len(parts) >= 5:
                self.display = tuple(int(v) for v in parts[1:5])

    def complete(self):
        return self.display is not None and len(self.buttons) == len(LABELS)

    def button_center(self, label):
        x, y, w, h = self.buttons[label]
        return (self.ox + x + w // 2, self.oy + y + h // 2)

    def display_box(self):
        """Screen-coordinate box of the numeric display strip.

        The display ITEM covers the expression line and the numeric
        strip below it; the strip is the lower part. Derived from the
        reported rect rather than from font metrics.
        """
        x, y, w, h = self.display
        strip_h = h * 2 // 3
        return (self.ox + x, self.oy + y + h - strip_h,
                self.ox + x + w, self.oy + y + h)


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
    """The display region, once the frame has settled.

    Every check here compares one capture against another, so a capture
    taken mid-paint fails a comparison with nothing wrong with it --
    which is what made this tool fail 4 runs in 6. The waiting lives in
    QMPSession.stable_pixels(), because every GUI tool that compares
    screendumps has the same exposure.
    """
    return qmp.stable_pixels(os.path.abspath(os.path.join(tmp, name)), box)


def run(dbg, qmp, tmp, shot_dir, res):
    # `gui spawn` rather than typing `run calculator` at a Terminal: the
    # kernel-space Terminal retired in M41's stage 0, and the ring-3 one
    # has no window yet when the injected keys would arrive.
    dbg.send(f"gui spawn {SPAWN_PATH}")

    # Poll for BOTH the window and the client's self-reported layout --
    # the ELF has to load and lay itself out before either is answerable.
    # Wait for the layout to be COMPLETE, not for the first layout line.
    # The two are different, and the difference is a flake: the client
    # logs one line per widget, so breaking on "a layout line arrived"
    # can leave the rest still in flight -- and the very next check
    # requires ALL of them. Measured at 1 run in 5 before this.
    deadline = time.time() + SPAWN_TIMEOUT_S
    win, lines, lay = None, [], None
    while time.time() < deadline:
        lines += dbg.logs("calculator:", clear=True)
        win = dbg.window(TITLE)
        if win:
            lay = Layout(win["content"], lines)
            if lay.complete():
                break
        time.sleep(0.3)
    res.check("Calculator runs as a ring-3 process with its own window", win is not None,
              f"no window titled {TITLE!r} within {SPAWN_TIMEOUT_S}s")
    if not win:
        return

    lay = lay or Layout(win["content"], lines)
    res.check("Calculator reports its own layout", lay.complete(),
              f"got {len(lay.buttons)}/{len(LABELS)} buttons, display={lay.display}")
    if not lay.complete():
        return
    box = lay.display_box()
    zero = display_pixels(qmp, tmp, "calc_zero.png", box)

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-calculator.png")))

    # --- the buttons are actually DRAWN -------------------------------
    #
    # This check exists because its absence shipped a Calculator with no
    # visible buttons at all. The layout placed them and hit-testing
    # worked, so every existing check passed: they all assert that
    # clicking a button CHANGES THE DISPLAY, which it did. "A click has
    # an effect" is not "the button is visible", and nothing was
    # checking the second.
    #
    # Sampled a few pixels inside a button's face, away from its centred
    # label, plus a control point on the panel background that must NOT
    # be button-coloured.
    from PIL import Image
    shot = os.path.abspath(os.path.join(tmp, "calc_faces.png"))
    qmp.screenshot(shot)
    with Image.open(shot) as im:
        px = im.convert("RGB")

        def face(label):
            x, y, w, h = lay.buttons[label]
            return px.getpixel((lay.ox + x + 3, lay.oy + y + 3))

        digit = face("7")          # UTHEME_BUTTON_BG
        operator = face("/")       # the operator tint
        # Between the display and the grid: panel background, no button.
        dx, dy, dw, dh = lay.display
        gap = px.getpixel((lay.ox + dx + 2, lay.oy + dy + dh + 1))

    res.check("the buttons are actually drawn, not just clickable",
              digit != gap and operator != gap,
              f"digit face {digit}, operator face {operator}, panel {gap} "
              "-- a button face indistinguishable from the panel means nothing painted them")
    res.check("operator buttons are tinted differently from digits",
              operator != digit, f"both {digit}")

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
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "calculator_client_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
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
