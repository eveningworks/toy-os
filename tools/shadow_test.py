#!/usr/bin/env python3
"""Drop shadows under windows and menus (userland/wm/wm_shadow.c), and
the `desktop.shadows` setting that turns them off.

Reads PIXEL VALUES beside a window's edges rather than looking at a
screenshot (CLAUDE.md): a focused window must darken the desktop just
outside its bottom edge and the darkening must FADE with distance; an
inactive window must darken it LESS than a focused one at the same
distance; a Toykit menu popup must darken what is beside it; and with
the setting off, the same pixel beside the same edge must match a
control pixel farther out. Every check samples a control that should
not have changed as well -- a pixel well outside any shadow's reach.

The windows are placed by `gui drag` over the plain wallpaper band low
on the screen, where the desktop colour is one flat value, so a shadow
reads as a luminance drop and not as a wallpaper gradient.

    python3 tools/vm.py start
    python3 tools/shadow_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                  # noqa: E402
import port_guard                                # noqa: E402
from harness import Results  # noqa: E402

FAR = 60   # px past an edge: beyond any shadow's reach (~1.8 line heights)


Result = Results


def lum(px):
    return (px[0] * 299 + px[1] * 587 + px[2] * 114) // 1000


def shot(qmp, path):
    from PIL import Image
    qmp.screenshot(path)
    return Image.open(path).convert("RGB")


def below(im, w, dx_frac, d):
    """Luminance `d` px below the window's bottom edge, at a fraction of its width."""
    x = w["x"] + int(w["w"] * dx_frac)
    return lum(im.getpixel((x, w["y"] + w["h"] + d)))


def run(dbg, qmp, tmp, res):
    dbg.open_app("Notepad")
    dbg.settle(1.0)
    dbg.open_app("Calculator")
    dbg.settle(1.5)
    np_ = dbg.window("untitled")
    calc = dbg.window("Calculator")
    res.check("two windows are open", np_ is not None and calc is not None)
    if not (np_ and calc):
        return
    # Move both over the flat lower wallpaper band, side by side, well
    # apart so neither's shadow reaches the other.
    dbg.send(f"gui drag {np_['x'] + 200} {np_['y'] + 10} 330 210")
    dbg.settle(0.8)
    dbg.send(f"gui drag {calc['x'] + 100} {calc['y'] + 10} 900 210")
    dbg.settle(0.8)
    # Click Calculator's title so IT is focused and Notepad is inactive.
    calc = dbg.window("Calculator")
    dbg.send(f"gui click {calc['x'] + 60} {calc['y'] + 10}")
    dbg.settle(0.8)
    np_ = dbg.window("untitled")
    calc = dbg.window("Calculator")
    res.check("Calculator is focused and Notepad is not",
              calc["focused"] and not np_["focused"],
              f"calc={calc['focused']} notepad={np_['focused']}")

    im = shot(qmp, os.path.join(tmp, "shadow_on.png"))
    # Control: the desktop far below both windows, at each sample column.
    far_c = below(im, calc, 0.5, FAR)
    far_n = below(im, np_, 0.5, FAR)
    near_c = [below(im, calc, 0.5, d) for d in (2, 6, 12)]
    near_n = [below(im, np_, 0.5, d) for d in (2, 6, 12)]
    res.check("the focused window darkens the desktop just below its bottom edge",
              near_c[0] < far_c - 8, f"near={near_c} far={far_c}")
    res.check("...and the darkening fades with distance",
              near_c[0] <= near_c[1] <= near_c[2] <= far_c, f"near={near_c} far={far_c}")
    res.check("the inactive window darkens it too, but less than the focused one",
              far_n - 3 > near_n[0] > near_c[0] - (far_c - far_n),
              f"inactive near={near_n} far={far_n}; focused near={near_c} far={far_c}")
    # Beside a straight vertical edge as well, mid-height.
    row = calc["y"] + calc["h"] // 2
    side = [lum(im.getpixel((calc["x"] + calc["w"] + d, row))) for d in (2, 6, 12, FAR)]
    res.check("the desktop right of the focused window darkens and fades",
              side[0] < side[3] - 8 and side[0] <= side[1] <= side[2] <= side[3], f"right={side}")

    # A Toykit menu: Notepad's File menu is a popup window with a small
    # shadow. Compare the pixel just below its bottom edge before and
    # after opening it.
    dbg.send(f"gui click {np_['x'] + 20} {np_['y'] + 31}")
    dbg.settle(0.8)
    pop = [w for w in dbg.windows() if w.get("popup")]
    res.check("the File menu opened as a popup window", len(pop) == 1, f"popups={len(pop)}")
    if pop:
        p = pop[0]
        im2 = shot(qmp, os.path.join(tmp, "shadow_menu.png"))
        x = p["x"] + p["w"] // 2
        under = lum(im2.getpixel((x, p["y"] + p["h"] + 2)))
        before = lum(im.getpixel((x, p["y"] + p["h"] + 2)))
        res.check("the menu darkens what is just below it", under < before - 6,
                  f"before={before} with menu={under}")
    dbg.send(f"gui click {np_['x'] + 20} {np_['y'] + 31}")
    dbg.settle(0.5)

    # Off: the same pixel beside the same edge matches the control.
    try:
        dbg.send("sh config set desktop.shadows off")
        dbg.settle(1.2)
        im3 = shot(qmp, os.path.join(tmp, "shadow_off.png"))
        off_near = below(im3, calc, 0.5, 2)
        off_far = below(im3, calc, 0.5, FAR)
        res.check("desktop.shadows=off: the pixel below the edge matches the desktop",
                  abs(off_near - off_far) <= 2, f"near={off_near} far={off_far}")
        res.check("...and the control pixel itself did not move",
                  abs(off_far - far_c) <= 2, f"off far={off_far} on far={far_c}")
    finally:
        dbg.send("sh config set desktop.shadows on")
        dbg.settle(0.8)
    im4 = shot(qmp, os.path.join(tmp, "shadow_back.png"))
    res.check("desktop.shadows=on again: the shadow is back",
              below(im4, calc, 0.5, 2) < below(im4, calc, 0.5, FAR) - 8)

    stacked(dbg, qmp, tmp, res, near_n)


def stacked(dbg, qmp, tmp, res, single):
    """Windows EXACTLY on top of each other cast one window's shadow.

    Shadows combine by the darkest, not the product (wm_shadow.h): three
    stacked inactive Notepads multiplied it to (1-a)^3 and ten made a
    solid black ring. `single` is one inactive Notepad's luminance at
    2/6/12 px below its edge, from the first phase.
    """
    first = dbg.window("untitled")
    for _ in range(2):
        dbg.open_app("Notepad")
        dbg.settle(1.5)
        top = [w for w in dbg.windows() if w["title"] == "untitled" and w["focused"]]
        if not top:
            res.check("a second Notepad opened, focused", False)
            return
        t = top[0]
        dbg.send(f"gui drag {t['x'] + 200} {t['y'] + 10} "
                 f"{first['x'] + 200} {first['y'] + 10}")
        dbg.settle(0.8)
    rects = {(w["x"], w["y"], w["w"], w["h"]) for w in dbg.windows() if w["title"] == "untitled"}
    res.check("three Notepads sit exactly on one rect", len(rects) == 1, f"rects={rects}")
    if len(rects) != 1:
        return
    # Calculator focused again, so all three are inactive like `single`.
    calc = dbg.window("Calculator")
    dbg.send(f"gui click {calc['x'] + 60} {calc['y'] + 10}")
    dbg.settle(0.8)
    im = shot(qmp, os.path.join(tmp, "shadow_stacked.png"))
    w = dbg.window("untitled")
    near = [below(im, w, 0.5, d) for d in (2, 6, 12)]
    res.check("three stacked windows darken no more than one",
              all(abs(a - b) <= 2 for a, b in zip(near, single)),
              f"stacked={near} one window={single}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "shadow_test")
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
    print(f"\nshadow_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
