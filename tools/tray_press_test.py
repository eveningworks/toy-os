#!/usr/bin/env python3
"""The notification area's feedback: a rounded fill on hover, a
different one while held, and nothing that latches.

WHAT IS UNDER TEST
------------------
A tray item (userland/wm/wm_tray.c) draws a rounded fill behind itself
while the pointer rests on it and another while the left button is held
on it -- Windows 11's and Plasma's rule, and the taskbar's own buttons'.
(Press-only, the macOS menu-bar rule, until the 2026-09-29 redesign.)

The clock is the item driven here because it is the one item that is
always registered, whatever hardware the guest has.

WHAT THIS ASSERTS THAT AN "IT RESPONDS" CHECK WOULD NOT
-------------------------------------------------------
1. THE PILL IS DRAWN, as pixel values: the item's left padding strip
   (four columns the glyphs never reach, so the ticking seconds cannot
   move them) must darken while the button is held and be byte-identical
   to rest otherwise.

2. HOVER IS DRAWN, AND IS NOT THE PRESS. The cursor is PARKED on the
   item with the button up -- DebugConsole.warp_cursor, never `gui move`,
   which lasts one wm_run() iteration -- and the strip must change, but
   `tray_pressed` must not, and the held fill must differ from it again.

3. IT IS A PILL, NOT A BLOCK. The fill's top-left corner pixel must stay
   nearer the panel's colour than the fill's interior, which is what
   rounding means and what a plain ugfx_fill_rect would fail.

4. IT DISARMS WHEN DRAGGED OFF, and re-arms on re-entry: the
   arm-on-press rule every control here follows
   (docs/gui-guidelines.md), read from `gui taskbar --json`'s
   `tray_pressed` as well as from the pixels.

5. THE NEIGHBOUR STAYS PUT. An empty patch of the strip left of the tray
   must be byte-identical between the rest and held captures -- half the
   assertion is what did not change (CLAUDE.md).

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/tray_press_test.py
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



_res = Results()
check = _res.check
checks = _res.rows


def pressed_id(dbg):
    return dbg.json("gui taskbar --json")["tray_pressed"]


def lum(rgb):
    return (rgb[0] * 30 + rgb[1] * 59 + rgb[2] * 11) // 100


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "tray_press_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.join(outdir, n)  # noqa: E731

    try:
        from PIL import Image
    except ImportError:
        print("tray_press_test: SKIP -- Pillow not installed, and every "
              "assertion here is a pixel value")
        return 0

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("tray feedback (the clock: a fill on hover, another while held)")

    # A popup left open by an earlier tool would be dismissed by this
    # tool's first press, which is a different code path from the one
    # under test.
    dbg.send("gui click 640 300")
    dbg.settle(); time.sleep(0.3)

    cal = dbg.json("gui calendar --json")
    clock = cal.get("clock")
    if not check("the tray reports the clock's box", bool(clock) and clock["w"] > 0,
                 str(clock)):
        return report()

    # THE PADDING STRIP, four columns wide: the item's box starts
    # TRAY_PAD (8px) left of its glyphs (wm_tray.c's walk), so this band
    # is inside the fill and outside the text -- which is what makes it
    # immune to the seconds ticking underneath the comparison.
    bx, by, bh = clock["x"], clock["y"], clock["h"]
    cy = by + bh // 2
    strip = (bx, cy - 3, bx + 4, cy + 3)
    # A patch of empty strip well left of the tray: the control.
    bar = dbg.json("gui taskbar --json")
    quiet = (bar["tray_x"] - 40, cy - 3, bar["tray_x"] - 20, cy + 3)

    away = (640, 300)

    def sample(name):
        px = qmp.stable_pixels(shot(name), box=strip)
        return px, Image.open(shot(name)).convert("RGB")

    # --- rest: the pointer nowhere near the strip ----------------------
    dbg.warp_cursor(qmp, *away)
    dbg.settle(); time.sleep(0.3)
    rest, rest_im = sample("tray_rest.png")
    check("nothing is pressed at rest", pressed_id(dbg) == -1,
          f"tray_pressed={pressed_id(dbg)}")

    # --- 2. hover is drawn, and is not a press ------------------------
    dbg.warp_cursor(qmp, clock["cx"], clock["cy"])
    dbg.settle(); time.sleep(0.4)
    hovered, _ = sample("tray_hover.png")
    check("the cursor ON the item draws its hover fill", hovered != rest,
          "the strip did not change under the cursor" if hovered == rest else "changed")
    check("...and the WM agrees nothing is pressed", pressed_id(dbg) == -1)

    # --- 1. held: the pill -------------------------------------------
    qmp.mouse_down()
    time.sleep(0.4)
    held_id = pressed_id(dbg)
    check("holding the button arms the item", held_id >= 0, f"tray_pressed={held_id}")
    held, held_im = sample("tray_held.png")
    check("...and the padding strip is repainted, unlike the hover",
          held != rest and held != hovered,
          "same as rest or hover" if held in (rest, hovered) else "pixels changed")

    rest_l = lum(rest_im.getpixel((bx + 2, cy)))
    held_l = lum(held_im.getpixel((bx + 2, cy)))
    # Darker, not merely different: uui_state_bg() washes toward black on
    # a light panel and toward white on a dark one, so the direction is
    # read from the panel rather than assumed.
    panel_is_light = rest_l > 128
    check("...and the fill moves away from the panel's own colour",
          (held_l < rest_l - 4) if panel_is_light else (held_l > rest_l + 4),
          f"panel {rest_l} -> held {held_l}")

    # --- 3. it is a PILL ----------------------------------------------
    # The fill spans the buttons' box (`btn_y` in the report), so its
    # top-left pixel is the corner -- sampled there and not above it,
    # where the strip itself would pass this trivially.
    corner = lum(held_im.getpixel((bx, bar["btn_y"])))
    interior = lum(held_im.getpixel((bx + 2, cy)))
    check("the fill's corner is rounded, not square",
          abs(corner - rest_l) < abs(interior - rest_l),
          f"corner {corner}, interior {interior}, panel {rest_l}")

    # --- 5. the neighbour -------------------------------------------
    check("an empty patch of the strip did not change",
          rest_im.crop(quiet).tobytes() == held_im.crop(quiet).tobytes(),
          str(quiet))

    # --- 4. dragged off, and back ------------------------------------
    dbg.warp_cursor(qmp, *away)
    dbg.settle(); time.sleep(0.4)
    check("dragging off the item disarms it", pressed_id(dbg) == -1,
          f"tray_pressed={pressed_id(dbg)}")
    off, _ = sample("tray_dragged_off.png")
    check("...and the fill is gone from the pixels", off == rest,
          "restored" if off == rest else "the strip is still filled")

    dbg.warp_cursor(qmp, clock["cx"], clock["cy"])
    dbg.settle(); time.sleep(0.4)
    check("coming back onto it re-arms", pressed_id(dbg) == held_id,
          f"tray_pressed={pressed_id(dbg)}")

    # --- release ------------------------------------------------------
    qmp.mouse_up()
    time.sleep(0.4)
    check("releasing clears it", pressed_id(dbg) == -1,
          f"tray_pressed={pressed_id(dbg)}")

    # Nothing latches: the press opened the calendar, and once the pointer
    # leaves, the item must go back to rest while that popup is up. The
    # pointer leaves DOWNWARD-LEFT along the strip, not into the popup.
    still_open = dbg.json("gui calendar --json")["open"]
    check("the press opened the calendar (so the item is lit BY the press, "
          "not by the popup)", still_open)
    dbg.warp_cursor(qmp, bar["tray_x"] - 60, cy)
    dbg.settle(); time.sleep(0.4)
    lit, _ = sample("tray_after_release.png")
    check("...and the item is at rest again anyway", lit == rest,
          "restored" if lit == rest else "the fill outlived the press")

    # Leave the desktop as it was found.
    dbg.send("gui click 640 300")
    dbg.settle()
    dbg.warp_cursor(qmp, *away)
    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\ntray_press_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
