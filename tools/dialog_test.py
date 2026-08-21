#!/usr/bin/env python3
"""tools/dialog_test.py -- verify the confirm dialog's Yes/No buttons.

    python3 tools/vm.py start        # or --disk a copy
    python3 tools/dialog_test.py
    echo $?                          # 0 = every check passed

WHAT IT CHECKS, AND WHY BY PIXEL VALUE
--------------------------------------
The reported bug was that Yes/No "won't react anyway graphically". A
screenshot only proves something drew; the PIXEL VALUES prove the hover
wash actually moved -- and that the button next to it, which should not
have changed, stayed put. That pairing is half the assertion
(docs/gui-guidelines.md). It also checks the cancel path: press Yes, drag
off, release, and the dialog must still be open.

THREE TRAPS THIS ENCODES
------------------------
  1. **Hover needs the REAL cursor, parked.** `gui move` only holds for
     one WM iteration -- injected input overrides the mouse for that
     iteration and then the real pointer takes over -- so hover is
     recomputed away before anything can look at it. This uses
     DebugConsole.warp_cursor(), which drives the real PS/2 cursor and
     CONFIRMS where it landed; QMPSession.goto() alone is open-loop and
     measured a large jump landing about a third of the way.
  2. **Don't sample the pixel under the cursor.** The sprite is drawn
     down-and-right from its hotspot with a black outline, so probing the
     hover point measures the cursor. The first version of this check
     "passed" by reading (0, 0, 0) -- pure cursor.
  3. **Use "Exit to shell", not "Shutdown".** Both open the identical
     dialog; committing Yes on Shutdown powers the machine off mid-test.

The buttons are located by scanning for THEME_BUTTON_BG rather than by
hardcoded offsets, because the dialog sizes itself to its message and any
reword would move them.
"""
import argparse
import sys, os, tempfile, time
sys.path.insert(0, "tools")
from qmp_test import QMPSession
from gui_debug import DebugConsole, enter_gui
from PIL import Image

# Scratch screenshots go to a temp directory, NOT the working tree.
# This used to default to "." and so rewrote dlg-rest.png /
# dlg-hover-yes.png in the repo root on every run -- which meant a
# routine test showed up as a dirty git status, and (worse) those two
# stale PNGs had been committed once and were silently re-committed
# whenever someone ran the tool before staging. Pass a directory as
# argv[1] to keep them somewhere.
# Screenshot scratch dir. Was sys.argv[1], which collided with the
# flags added below -- gui_regress passes --sock first, so OUT
# would have become the literal string "--sock".
OUT = tempfile.gettempdir()
fails = []


def px(path, x, y):
    return Image.open(path).convert("RGB").getpixel((x, y))


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not ok:
        fails.append(name)


def main():
    global OUT
    # The same --sock/--qmp-port every other tool takes. Without them
    # this could only ever run against VM slot 0, so gui_regress -- which
    # hands each tool its own slot -- could not include it: it connected
    # to slot 0's socket, found nothing there, and timed out in six
    # seconds looking like a dialog bug.
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=".vm.serial")
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=OUT)
    args = ap.parse_args()
    OUT = args.tmp

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)

    st = dbg.json("gui taskbar --json")["start"]
    dbg.send(f"gui click {st['cx']} {st['cy']}")
    dbg.settle()
    x, y = dbg.menu_row("Exit to shell")
    dbg.send(f"gui click {x} {y}")
    dbg.settle()
    time.sleep(0.5)
    state = dbg.json("gui state --json")
    check("dialog opened", state["overlays"]["confirm_dialog"], str(state["overlays"]))
    if not state["overlays"]["confirm_dialog"]:
        return 1

    scr = state["screen"]
    cx, cy = scr["w"] // 2, scr["h"] // 2

    # Hover must be held across frames, so it goes through QMP's REAL
    # PS/2 mouse, not `gui move`: injected input overrides the mouse for
    # one WM iteration only, after which mouse_get_state() reports the
    # real pointer again and the hover is recomputed away.
    print('  parked at', dbg.warp_cursor(qmp, cx, cy - 200))
    a = os.path.abspath(f"{OUT}/dlg-rest.png")
    qmp.screenshot(a)

    # ASK the WM where the buttons are (`gui dialog --json`), rather than
    # scanning the button row for THEME_BUTTON_BG.
    #
    # The colour scan worked and was still wrong in the way this repo
    # keeps paying for: it assumed the dialog's palette AND that both
    # buttons sit on one row of a known width band. Both assumptions were
    # true only for a Yes/No dialog -- the force-quit dialog's "Force
    # Quit"/"Wait" are wider and differently spaced, so a tool written
    # this way measures one dialog and silently cannot measure another.
    info = dbg.json("gui dialog --json")
    btns = info.get("buttons", [])
    check("the WM reports both dialog buttons", len(btns) == 2, str(info))
    if len(btns) != 2:
        return 1

    runs = [(b["x"], b["x"] + b["w"] - 1) for b in btns]
    row = btns[0]["cy"]
    print(f"  buttons: {[(b['label'], b['cx'], b['cy']) for b in btns]}")

    yes_c = (btns[0]["cx"], btns[0]["cy"])
    no_c = (btns[1]["cx"], btns[1]["cy"])

    # Hover Yes.
    print('  hover at', dbg.warp_cursor(qmp, *yes_c))
    time.sleep(0.3)
    b = os.path.abspath(f"{OUT}/dlg-hover-yes.png")
    qmp.screenshot(b)

    # Sample to the LEFT of where the cursor is parked. The sprite is
    # drawn down-and-right from its hotspot and its outline is black, so
    # sampling the hover point itself measures the CURSOR, not the wash --
    # which is exactly the false positive this check first produced
    # (hover read as (0,0,0), i.e. pure cursor outline).
    yes_probe = (runs[0][0] + 4, row)
    no_probe = (runs[1][0] + 4, row)
    rest_yes, hov_yes = px(a, *yes_probe), px(b, *yes_probe)
    rest_no, hov_no = px(a, *no_probe), px(b, *no_probe)
    print(f"  Yes: rest {rest_yes} -> hover {hov_yes}")
    print(f"  No : rest {rest_no} -> hover {hov_no}   (must NOT change)")
    check("hovering Yes changes Yes", rest_yes != hov_yes)
    check("hovering Yes leaves No alone", rest_no == hov_no)

    # The cancel path: press Yes, drag off, release. The dialog must
    # still be open and nothing must have happened.
    dbg.send(f"gui drag {yes_c[0]} {yes_c[1]} {cx - 300} {cy - 250}")
    dbg.settle()
    time.sleep(0.4)
    st2 = dbg.json("gui state --json")
    check("press dragged off Yes did NOT commit",
          st2["overlays"]["confirm_dialog"], str(st2["overlays"]))

    # And No actually closes it.
    dbg.send(f"gui click {no_c[0]} {no_c[1]}")
    dbg.settle()
    time.sleep(0.4)
    st3 = dbg.json("gui state --json")
    check("No closes the dialog", not st3["overlays"]["confirm_dialog"],
          str(st3["overlays"]))

    print(f"\n=== {len(fails)} failed ===")
    for f in fails:
        print("  FAILED:", f)
    dbg.close()
    return 1 if fails else 0


sys.exit(main())
