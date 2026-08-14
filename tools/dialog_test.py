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
import sys, os, time
sys.path.insert(0, "tools")
from qmp_test import QMPSession
from gui_debug import DebugConsole
from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
fails = []


def px(path, x, y):
    return Image.open(path).convert("RGB").getpixel((x, y))


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not ok:
        fails.append(name)


def main():
    qmp = QMPSession(port=4445)
    qmp.send_text("gui"); qmp.send_key("ret"); time.sleep(2.0)
    dbg = DebugConsole(".vm.serial")

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

    # Find the two buttons by scanning the button row for the button
    # background colour, rather than guessing offsets: the dialog sizes
    # itself to its message, so hardcoding would break on a reword.
    img = Image.open(a).convert("RGB")
    btn_bg = (225, 225, 230)  # THEME_BUTTON_BG
    row = None
    for yy in range(cy, cy + 90):
        xs = [xx for xx in range(cx - 200, cx + 200) if img.getpixel((xx, yy)) == btn_bg]
        if len(xs) > 40:
            row, spans = yy, xs
            break
    if row is None:
        print("  FAIL  could not locate the button row by colour")
        return 1

    # Split the matched x's into two runs -- Yes and No.
    runs = []
    start = spans[0]
    for i in range(1, len(spans)):
        if spans[i] != spans[i - 1] + 1:
            runs.append((start, spans[i - 1]))
            start = spans[i]
    runs.append((start, spans[-1]))
    print(f"  button row y={row}, runs={runs}")
    check("found exactly two buttons", len(runs) == 2, str(runs))
    if len(runs) != 2:
        return 1

    yes_c = ((runs[0][0] + runs[0][1]) // 2, row)
    no_c = ((runs[1][0] + runs[1][1]) // 2, row)

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
