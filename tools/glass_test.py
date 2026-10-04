#!/usr/bin/env python3
"""Transparency (userland/wm/wm_glass.c): the taskbar, Start, menus and
windows drawn as glass, and every `desktop.transparency*` setting.

Reads PIXEL VALUES, each against a control taken with the setting off
or with the scene changed, so a check cannot pass by the screen merely
looking different:

  - CLEAR taskbar: the panel shows the scene in proportion to its
    opacity -- at 50 % it is twice as far from its own colour as at 75 %,
    whatever happens to be beneath it.
  - FROSTED vs CLEAR: across a window's edge under the Start menu, Clear
    shows a sharp step and Frosted spreads it out.
  - WALLPAPER: moving that window away changes nothing under the menu
    (only the wallpaper shows), while it does change Frosted.
  - the desktop's menu and an app's menu (a WIN_POPUP_GLASS client):
    their ground darkens toward the wallpaper behind them when on, and is
    the plain menu colour when off.
  - window modes: which window bodies stay pure white -- none, the
    focused one, or neither -- and the title bar against its solid colour;
    "while moving" holds a real button down and looks mid-drag.

Damage verification is on throughout, because FROSTED glass is part of
its rect's damage (wm_glass.h) and a missed growth leaves a stale blur
the verifier reports. Every setting is put back at the end.

    python3 tools/vm.py start
    python3 tools/glass_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                  # noqa: E402
import port_guard                                # noqa: E402
from harness import Results                      # noqa: E402

SURFACES = ("taskbar", "start", "menus", "window")
DEFAULTS = {"transparency": "off",
            **{f"transparency_{k}_glass": "frosted" for k in SURFACES},
            "transparency_taskbar": "75", "transparency_start": "85",
            "transparency_menus": "85", "transparency_windows": "titlebars",
            "transparency_window_opacity": "85", "transparency_moving": "off",
            "taskbar_float": "off"}


def lum(px):
    return (px[0] * 299 + px[1] * 587 + px[2] * 114) // 1000


class Glass:
    def __init__(self, dbg, qmp, tmp, res):
        self.dbg, self.qmp, self.tmp, self.res = dbg, qmp, tmp, res
        self.n = 0

    def glass(self, kind, **only):
        """Every surface's glass to `kind`, then any named one otherwise."""
        kv = {f"transparency_{k}_glass": kind for k in SURFACES}
        kv.update({f"transparency_{k}_glass": v for k, v in only.items()})
        self.set(**kv)

    def set(self, **kv):
        for k, v in kv.items():
            self.dbg.send(f"sh config set desktop.{k} {v}")
        self.dbg.settle(1.0)

    def shot(self, name):
        from PIL import Image
        self.n += 1
        p = os.path.join(self.tmp, f"glass_{self.n:02d}_{name}.png")
        self.qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    def click_away(self):
        # The desktop's empty right edge: closes any menu, focuses nothing new.
        st = self.dbg.json("gui state --json")
        self.dbg.send(f"gui click {st['screen']['w'] - 20} 200")
        self.dbg.settle(0.8)

    def open_start(self):
        s = self.dbg.json("gui taskbar --json")["start"]
        self.dbg.send(f"gui click {s['x'] + s['w'] // 2} {s['y'] + s['h'] // 2}")
        self.dbg.settle(1.0)
        return self.dbg.json("gui menu --json")

    def notepads(self):
        return [w for w in self.dbg.windows() if w["title"] == "untitled" and not w.get("popup")]

    def drag_to(self, w, x, y):
        self.dbg.send(f"gui drag {w['x'] + 150} {w['y'] + 10} {x + 150} {y + 10}")
        self.dbg.settle(1.0)


def taskbar(g, res):
    tb = g.dbg.json("gui taskbar --json")
    bar = tb["bar"]
    bar = ((bar >> 16) & 255, (bar >> 8) & 255, bar & 255)
    y = tb["panel"]["y"] + tb["panel"]["h"] // 2
    # Between the window buttons and the tray, where the strip is bare.
    x = tb["panel"]["x"] + tb["panel"]["w"] // 2
    g.set(transparency="off")
    off = g.shot("taskbar_off").getpixel((x, y))
    res.check("off: the taskbar is its own colour", off == bar, f"px={off} bar={bar}")
    g.set(transparency="on", transparency_taskbar="75")
    g.glass("clear")
    p75 = g.shot("taskbar_75").getpixel((x, y))
    g.set(transparency_taskbar="50")
    p50 = g.shot("taskbar_50").getpixel((x, y))
    d75 = [p75[c] - bar[c] for c in range(3)]
    d50 = [p50[c] - bar[c] for c in range(3)]
    c = max(range(3), key=lambda k: abs(d50[k]))
    res.check("clear 75 %: the scene shows through the taskbar", abs(d75[c]) >= 4,
              f"px={p75} bar={bar}")
    res.check("...and at 50 % twice as far from the bar's colour",
              abs(d50[c] - 2 * d75[c]) <= 3, f"75%={d75} 50%={d50}")
    g.set(transparency_taskbar="100")
    p100 = g.shot("taskbar_100").getpixel((x, y))
    res.check("100 %: the taskbar is opaque again", p100 == bar, f"px={p100}")

    # FLOATING, it casts a shadow -- which must not be shaded UNDER the
    # see-through panel, where it showed as a dark band along its edges.
    g.set(taskbar_float="on", transparency_taskbar="40")
    tb = g.dbg.json("gui taskbar --json")
    p = tb["panel"]
    im = g.shot("taskbar_float40")
    col = p["x"] + p["w"] // 2
    edge = [lum(im.getpixel((col, p["y"] + d))) for d in (3, 5, p["h"] - 6, p["h"] - 4)]
    mid = lum(im.getpixel((col, p["y"] + p["h"] // 2)))
    res.check("floating at 40 %: no shadow band inside the panel's edges",
              all(abs(e - mid) <= 4 for e in edge), f"edges={edge} middle={mid}")
    # ...nor in its corners: a pixel well inside each arc, against the
    # middle of the same end of the panel.
    k = max(2, p["r"] // 2)
    corners = {}
    for cx, cy in ((p["x"] + k, p["y"] + k), (p["x"] + p["w"] - 1 - k, p["y"] + k),
                   (p["x"] + k, p["y"] + p["h"] - 1 - k),
                   (p["x"] + p["w"] - 1 - k, p["y"] + p["h"] - 1 - k)):
        corners[(cx, cy)] = lum(im.getpixel((cx, cy))) - lum(im.getpixel((cx, p["y"] + p["h"] // 2)))
    res.check("...and none in its corners, inside the arc",
              all(abs(d) <= 4 for d in corners.values()), f"corner minus same column's middle={corners}")
    g.set(taskbar_float="off", transparency_taskbar="75")


def row_profile(im, y, x0, x1):
    return [lum(im.getpixel((x, y))) for x in range(x0, x1)]


def start_menu(g, res):
    """Start over the left edge of a white Notepad."""
    g.dbg.open_app("Notepad")
    g.dbg.settle(1.5)
    np_ = g.notepads()[0]
    g.drag_to(np_, 420, 150)
    np_ = g.notepads()[0]
    sm = g.open_start()
    res.check("the Start menu opened", sm.get("open"), "")
    g.click_away()
    edge = np_["x"]
    # The app pane below its rows AND above the window's bottom: a row
    # fixed to the menu alone fell under Notepad once the menu grew.
    y = min(sm["y"] + sm["h"] - 120, np_["y"] + np_["h"] - 30)
    inside = sm["x"] + 210 < edge < sm["x"] + sm["w"] - 40
    res.check("the window's edge lies under the Start menu's pane", inside,
              f"edge={edge} menu={sm['x']}..{sm['x'] + sm['w']}")
    if not inside:
        return
    x0, x1 = edge - 40, edge + 40
    prof = {}
    for kind in ("clear", "frosted", "wallpaper"):
        g.glass(kind)
        g.open_start()
        prof[kind] = row_profile(g.shot(f"start_{kind}"), y, x0, x1)
        g.click_away()

    def jump(p):
        return max(abs(p[i + 1] - p[i]) for i in range(len(p) - 1))
    res.check("clear: the window's edge is a sharp step through the menu",
              jump(prof["clear"]) >= 12, f"max step={jump(prof['clear'])}")
    res.check("frosted: the same edge is spread out",
              jump(prof["frosted"]) * 3 <= jump(prof["clear"]),
              f"frosted={jump(prof['frosted'])} clear={jump(prof['clear'])}")

    # EACH SURFACE ITS OWN: Start alone Clear, everything else Frosted.
    g.glass("frosted", start="clear")
    g.open_start()
    own = row_profile(g.shot("start_own_clear"), y, x0, x1)
    g.click_away()
    res.check("Start's own glass setting: Clear under Start while the rest are Frosted",
              jump(own) >= 12, f"max step={jump(own)}")

    # Move the window away: Wallpaper glass must not notice, Frosted must.
    g.drag_to(g.notepads()[0], 760, 80)
    after = {}
    for kind in ("frosted", "wallpaper"):
        g.glass(kind)
        g.open_start()
        after[kind] = row_profile(g.shot(f"start_{kind}_moved"), y, x0, x1)
        g.click_away()
    dw = max(abs(a - b) for a, b in zip(prof["wallpaper"], after["wallpaper"]))
    df = max(abs(a - b) for a, b in zip(prof["frosted"], after["frosted"]))
    res.check("wallpaper: a window beneath does not show through", dw <= 1, f"max change={dw}")
    res.check("...where frosted does show it (the control)", df >= 6, f"max change={df}")
    g.glass("frosted")


def desktop_menu(g, res):
    # Low on the left, clear of the window the last phase left at the right.
    st = g.dbg.json("gui state --json")
    rx, ry = 300, st["screen"]["h"] - st["taskbar_h"] - 60
    vals = {}
    for on in ("off", "on"):
        g.set(transparency=on)
        g.dbg.send(f"gui rclick {rx} {ry}")
        g.dbg.settle(1.0)
        cm = g.dbg.json("gui ctxmenu --json")
        row = [r for r in cm["rows"] if r["label"] == "New folder"]
        if not cm.get("open") or not row:
            res.check(f"{on}: the desktop menu opened", False, str(cm)[:200])
            return
        px = (cm["x"] + cm["w"] - 12, row[0]["cy"])
        vals[on] = g.shot(f"ctx_{on}").getpixel(px)
        g.click_away()
    res.check("the desktop menu's ground shows the wallpaper through it when on",
              lum(vals["on"]) <= lum(vals["off"]) - 6, f"off={vals['off']} on={vals['on']}")


def app_menu(g, res):
    """Notepad low on the screen, so its File menu opens UP over the desktop."""
    np_ = g.notepads()[0]
    st = g.dbg.json("gui state --json")
    g.drag_to(np_, 700, st["screen"]["h"] - st["taskbar_h"] - 110)
    np_ = g.notepads()[0]
    vals = {}
    for on in ("off", "on"):
        g.set(transparency=on)
        g.dbg.send(f"gui click {np_['content']['x'] + 15} {np_['content']['y'] + 10}")
        g.dbg.settle(1.0)
        pop = [w for w in g.dbg.windows() if w.get("popup")]
        if not pop:
            res.check(f"{on}: Notepad's File menu opened", False)
            return
        p = pop[0]
        above = p["y"] + 6 < np_["y"]
        res.check(f"{on}: the File menu opened up, its top over the desktop", above,
                  f"menu y={p['y']}..{p['y'] + p['h']} window y={np_['y']}")
        vals[on] = g.shot(f"appmenu_{on}").getpixel((p["x"] + p["w"] - 10, p["y"] + 6))
        g.click_away()               # a press outside dismisses the menu
    res.check("an app's menu (WIN_POPUP_GLASS) shows the desktop through its ground",
              lum(vals["on"]) <= lum(vals["off"]) - 6, f"off={vals['off']} on={vals['on']}")


def windows(g, res):
    # Each drag takes the FOCUSED window, which is on top, so its title
    # bar is the one under the press: the new one right, then the other left.
    g.dbg.open_app("Notepad")
    g.dbg.settle(1.5)
    g.drag_to([w for w in g.notepads() if w["focused"]][0], 680, 60)
    other = [w for w in g.notepads() if not w["focused"]][0]
    g.dbg.send(f"gui click {other['x'] + 150} {other['y'] + 10}")
    g.dbg.settle(0.8)
    g.drag_to([w for w in g.notepads() if w["focused"]][0], 40, 60)
    ws = sorted(g.notepads(), key=lambda w: w["x"])
    left, right = ws[0], ws[1]
    # Focus the RIGHT one by its title.
    g.dbg.send(f"gui click {right['x'] + 120} {right['y'] + 10}")
    g.dbg.settle(0.8)
    ws = sorted(g.notepads(), key=lambda w: w["x"])
    left, right = ws[0], ws[1]
    res.check("the right window is focused", right["focused"] and not left["focused"])

    def body(im, w):
        return im.getpixel((w["content"]["x"] + w["content"]["w"] // 2,
                            w["content"]["y"] + w["content"]["h"] // 2))

    def title(im, w):
        return im.getpixel((w["x"] + w["w"] // 2, w["y"] + 4))

    g.set(transparency="off")
    im = g.shot("win_off")
    solid = title(im, right)
    res.check("off: both bodies are pure white", body(im, left) == body(im, right) == (255, 255, 255))
    g.set(transparency="on", transparency_windows="none")
    im = g.shot("win_none")
    res.check("none: both bodies are pure white, the title bar solid",
              body(im, left) == body(im, right) == (255, 255, 255) and title(im, right) == solid,
              f"title={title(im, right)} solid={solid}")
    g.set(transparency_windows="titlebars")
    im = g.shot("win_titlebars")
    res.check("title bars: the bodies stay white", body(im, left) == body(im, right) == (255, 255, 255))
    res.check("...and the focused title bar shows the desktop through it",
              title(im, right) != solid, f"title={title(im, right)} solid={solid}")
    g.set(transparency_windows="inactive")
    im = g.shot("win_inactive")
    res.check("inactive: the focused body is white, the other see-through",
              body(im, right) == (255, 255, 255) and lum(body(im, left)) <= 248,
              f"focused={body(im, right)} inactive={body(im, left)}")
    g.set(transparency_windows="all")
    im = g.shot("win_all")
    res.check("all: both bodies are see-through",
              lum(body(im, right)) <= 248 and lum(body(im, left)) <= 248,
              f"focused={body(im, right)} inactive={body(im, left)}")

    # WHILE MOVING: a REAL button held on the title bar, the frame taken
    # mid-drag. An injected `gui drag` releases in the same breath.
    g.set(transparency_windows="none", transparency_moving="on")
    g.dbg.send(f"gui warp {right['x'] + 120} {right['y'] + 10}")
    g.dbg.settle(0.5)
    g.qmp.mouse_down()
    try:
        time.sleep(0.3)
        g.qmp.move_rel(6, 0)
        time.sleep(0.6)
        held = g.dbg.json("gui state --json")["dragging"]
        im = g.shot("win_moving")
        moved = [w for w in g.notepads() if w["focused"]][0]
    finally:
        g.qmp.mouse_up()
    g.dbg.settle(0.8)
    res.check("while moving: the window is being dragged", held >= 0, f"dragging={held}")
    res.check("...and its body is see-through mid-drag", lum(body(im, moved)) <= 248,
              f"body={body(im, moved)}")
    im = g.shot("win_dropped")
    moved = [w for w in g.notepads() if w["focused"]][0]
    res.check("...and solid again once dropped", body(im, moved) == (255, 255, 255),
              f"body={body(im, moved)}")


def run(dbg, qmp, tmp, res):
    g = Glass(dbg, qmp, tmp, res)
    dbg.damage_verify(True)
    try:
        taskbar(g, res)
        start_menu(g, res)
        desktop_menu(g, res)
        g.set(transparency="on")
        app_menu(g, res)
        windows(g, res)
        bugs = dbg.damage_bugs()
        res.check("the damage verifier saw nothing stale", bugs == [], "; ".join(bugs)[:400])
    finally:
        dbg.damage_verify(False)
        g.set(**DEFAULTS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "glass_test")
    tmp = args.logs or "/tmp"
    os.makedirs(tmp, exist_ok=True)

    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, tmp, res)
    finally:
        dbg.close()
    return res.finish("glass_test")


if __name__ == "__main__":
    sys.exit(main())
