#!/usr/bin/env python3
"""The Terminal's screen effect (userland/ui/ucrt.c): off, toggled, set
from Options, saved, and a click under the curve landing on the cell
the glass SHOWS.

WHAT IT CHECKS, and what a broken version would still pass:

  - off by default: a blank band of the grid is ONE colour, the page's
    -- an effect drawn while "off" fails here;
  - Ctrl+Shift+E turns it on, and the same blank band now repeats a
    darker row at the scanline pitch the app derives from its font
    (cell height / 6) -- read from the pixels, so "the log says on" is
    not enough; a second Ctrl+Shift+E makes the band plain again;
  - Options > Screen effect > the Curved card > OK: the window turns
    curved, the grid's corner is BEZEL (dark, neutral, not the page),
    and /etc/terminal.conf says so, read back with `cat` rather than
    believed;
  - under that curve, a double-click on where the banner's first word
    APPEARS selects exactly that word (4 bytes). Where it appears comes
    from THIS file's own model of the warp, not the app's table, so the
    app is not checking itself.

THE POSITIVE CONTROL is mutate.py on terminal.c's point_at(), skipping
the ucrt_source_point() call: the double-click then lands on the cell
DRAWN under the pointer -- the prompt line below -- and the last check
fails. (Clicking the drawn position instead proves nothing: near the
corner it is bezel, and the app walks in to the same word.)
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                      # noqa: E402

TITLE = "Terminal"
SPAWN = "/bin/wm/apps/uterm"
BG = (0x23, 0x26, 0x29)          # Slate's page, terminal.c's default scheme
# ucrt.c's CURVE[UCRT_CURVE_STRONG]: barrel k, and the bezel's inset as a
# fraction of the rect's height. Kept in step by hand -- the model is
# deliberately NOT read from the app.
CURVE_K, CURVE_INSET = 0.11, 1.0 / 29

_res = Results()
check = _res.check


def field(dbg, name, n=1):
    for line in reversed(dbg.logs("uterm: layout cursor", clear=False)):
        p = line.split()
        if name in p:
            i = p.index(name)
            vals = [int(v) for v in p[i + 1:i + 1 + n]]
            return vals if n > 1 else vals[0]
    return None


def shot(qmp, tmp, name):
    from PIL import Image
    p = os.path.join(tmp, f"crt_{os.getpid()}_{name}.png")
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def column(im, x, y0, y1):
    return [im.getpixel((x, y))[:3] for y in range(y0, y1)]


def grid_rect(dbg):
    """Screen rect of the grid: the content area below the chrome."""
    w = dbg.window(TITLE)
    chrome = field(dbg, "chrome") or 0
    tabs = dbg.widgets(TITLE).get("tabs")
    ox = tabs["screen"]["x"] - tabs["x"]
    oy = tabs["screen"]["y"] - tabs["y"]
    cw = w["w"] - (ox - w["x"]) * 2
    ch = w["h"] - (oy - w["y"]) - (ox - w["x"])
    return ox, oy + chrome, cw, ch - chrome


def warp(w, h, x, y):
    """ucrt.c's warp(): where a point of the glass shows from."""
    inset = CURVE_INSET * h
    gw, gh = w - 2 * inset, h - 2 * inset
    u, v = (x - inset) / gw * 2 - 1, (y - inset) / gh * 2 - 1
    f = (1 + CURVE_K * (u * u + v * v)) / (1 + CURVE_K)
    return (u * f + 1) / 2 * w, (v * f + 1) / 2 * h


def shown_at(w, h, sx, sy):
    """The glass point whose warp is the source point (sx, sy)."""
    x, y = sx, sy
    for _ in range(60):
        wx, wy = warp(w, h, x, y)
        x, y = x + (sx - wx), y + (sy - wy)
    return x, y


def double_click(dbg, x, y):
    dbg.send(f"gui click {int(x)} {int(y)}")
    time.sleep(0.1)
    dbg.send(f"gui click {int(x)} {int(y)}")
    dbg.settle()


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "crt_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("Terminal screen effect (userland/ui/ucrt.c)")
    dbg.send("sh rm /etc/terminal.conf")
    dbg.logs("", clear=True)
    if not dbg.spawn(SPAWN, TITLE):
        check("the Terminal opens", False)
        return _res.finish("crt_test")
    time.sleep(2)
    dbg.settle()

    gx, gy, gw, gh = grid_rect(dbg)
    cw, ch = field(dbg, "cell", 2) or (8, 16)
    # A blank band: right half of the grid, below the banner and prompt.
    bx, by0, by1 = gx + gw * 3 // 4, gy + gh // 3, gy + gh // 3 + 8 * ch
    im = shot(qmp, args.tmp, "off")
    band = column(im, bx, by0, by1)
    check("off by default, a blank band is the page's colour alone",
          field(dbg, "effect") == 0 and set(band) == {BG}, f"{sorted(set(band))[:4]}")

    dbg.key("0x05", mods="shift")          # Ctrl+Shift+E
    time.sleep(1)
    dbg.settle()
    im = shot(qmp, args.tmp, "on")
    band = [sum(p) for p in column(im, bx, by0, by1)]
    period = max(2, (ch + 3) // 6)
    # The darkest row of each pitch must sit at the same phase every time.
    phases = {}
    for i, v in enumerate(band):
        phases.setdefault(i % period, []).append(v)
    means = {k: sum(v) / len(v) for k, v in phases.items()}
    dark = min(means, key=means.get)
    lift = min(m for k, m in means.items() if k != dark) - means[dark]
    steady = all(v == min(phases[dark]) or abs(v - means[dark]) < 4 for v in phases[dark])
    check("Ctrl+Shift+E: the band repeats a darker row at the scanline pitch",
          field(dbg, "effect") == 1 and lift >= 4 and steady,
          f"pitch {period}, dark phase {dark}, {lift:.1f} levels darker")
    first = [ln for ln in dbg.logs("uterm: effect ", clear=False) if " us" in ln]
    for ln in first[-2:]:
        print("    cost:", ln.split("uterm: ", 1)[1].strip())

    dbg.key("0x05", mods="shift")
    time.sleep(1)
    dbg.settle()
    band = column(shot(qmp, args.tmp, "off2"), bx, by0, by1)
    check("...and a second Ctrl+Shift+E makes it plain again", set(band) == {BG},
          f"{sorted(set(band))[:4]}")

    # --- Options > Screen effect > Curved > OK -----------------------
    tb = dbg.widgets(TITLE)["tb"]
    menu_btn = [ln for ln in dbg.logs("uterm: layout tbbtn 3 ", clear=False)]
    m = re.search(r"tbbtn 3 (\d+) (\d+) (\d+) (\d+)", menu_btn[-1]) if menu_btn else None
    if not check("the bar reports its menu button", m):
        return _res.finish("crt_test")
    ox, oy = tb["screen"]["x"] - tb["x"], tb["screen"]["y"] - tb["y"]
    bxm, bym, bwm, bhm = (int(v) for v in m.groups())
    dbg.logs("options:", clear=True)
    dbg.click(ox + bxm + bwm // 2, oy + bym + bhm // 2)
    dbg.send("gui key 0x91")               # Up twice: Exit, then Options...
    dbg.send("gui key 0x91")
    dbg.send("gui key 0x0d")
    time.sleep(2)
    dbg.send("gui key 0x92")               # the sidebar: Appearance -> Screen effect
    time.sleep(1)
    dbg.settle()
    ow = dbg.window("Terminal Options")
    card = [ln for ln in dbg.logs("options: layout looks.card 3 ", clear=False)]
    mc = re.search(r"card 3 (\d+) (\d+) (\d+) (\d+)", card[-1]) if card else None
    okl = [ln for ln in dbg.logs("options: layout ok ", clear=False)]
    mo = re.search(r"layout ok (\d+) (\d+) (\d+) (\d+)", okl[-1]) if okl else None
    if not check("Options opens on a Screen effect page with four looks", ow and mc and mo):
        return _res.finish("crt_test")
    # A secondary window reports no widgets, so its content origin is
    # its frame plus the frame-to-content offset the Terminal shows.
    tw = dbg.window(TITLE)
    pox = ow["x"] + (ox - tw["x"])
    poy = ow["y"] + (oy - tw["y"])
    x, y, w, h = (int(v) for v in mc.groups())
    dbg.click(pox + x + w // 2, poy + y + h // 3)
    x, y, w, h = (int(v) for v in mo.groups())
    dbg.click(pox + x + w // 2, poy + y + h // 2)
    time.sleep(2)
    dbg.settle()
    check("OK on the Curved card turns this window curved",
          field(dbg, "effect") == 1 and field(dbg, "curve") == 2,
          f"effect {field(dbg, 'effect')} curve {field(dbg, 'curve')}")
    conf = dbg.send("sh cat /etc/terminal.conf") or ""
    check("...and /etc/terminal.conf says so", "effect=on" in conf and "crt_curve=strong" in conf,
          conf.strip().replace("\n", " ")[-160:])
    gx, gy, gw, gh = grid_rect(dbg)
    im = shot(qmp, args.tmp, "curved")
    corner = im.getpixel((gx + 2, gy + 2))[:3]
    neutral = max(corner) - min(corner) <= 6 and max(corner) < 40
    check("...with the grid's corner on the bezel, not the page", neutral and corner != BG,
          f"corner {corner}")

    # --- a double-click where the word SHOWS ------------------------
    margin = field(dbg, "margin")
    cw, ch = field(dbg, "cell", 2)
    dx, dy = margin + 1.5 * cw, margin + 0.5 * ch       # "tosh", its second cell
    sx, sy = shown_at(gw, gh, dx, dy)
    print(f"    the banner's 'tosh' is drawn at {dx:.0f},{dy:.0f} and shows at {sx:.0f},{sy:.0f}")
    check("...and the curve moves it by more than a cell, or the next check is moot",
          abs(sx - dx) > cw and abs(sy - dy) > ch, f"{sx - dx:.0f},{sy - dy:.0f} px")
    double_click(dbg, gx + sx, gy + sy)
    got = field(dbg, "selbytes")
    check("under the curve, a double-click where 'tosh' shows selects it", got == 4,
          f"selbytes {got}")

    dbg.key("0x05", mods="shift")          # leave the window plain
    dbg.send("sh rm /etc/terminal.conf")
    w = dbg.window(TITLE)
    if w:
        dbg.send(f"gui close {w['z']}")
    return _res.finish("crt_test")


if __name__ == "__main__":
    sys.exit(main())
