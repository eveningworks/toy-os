#!/usr/bin/env python3
"""Visible regions (userland/wm/wm_render.c): a window covered by an
OPAQUE window is not drawn, and one under a SEE-THROUGH window still is.

Two Notepads, A under B, and three checks, each with its control:

  - COVERED: B maximized hides all of A, shadow included. Over a burst of
    frames whose damage lands on A (hovering B's toolbar above A), the
    compositor's own counters (`gui compositor --json`'s `windows`) must
    show A CULLED and never drawn: at most one window drawn per frame.
    The control is the burst itself -- `culled_total` must be non-zero,
    or no frame ever touched A and the check measured nothing.
  - SEE-THROUGH: B restored and set over A's title bar, every window glass
    (`desktop.transparency_windows all`, Clear). A pixel of B's white body
    over A must differ from the same pixel with A elsewhere -- A shows
    through, so it was drawn -- and be pure white with transparency off
    (the control that the pixel is B's body at all).
  - the damage verifier stays quiet throughout: its reference render
    culls NOTHING, so a window wrongly left out is reported as a miss.

    python3 tools/vm.py start
    python3 tools/occlusion_test.py
    python3 tools/vm.py stop

Positive controls (tools/mutate.py): disabling the cull reddens COVERED;
counting a see-through window as opaque reddens the verifier.
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

GLASS = {"transparency": "on", "transparency_windows": "all",
         "transparency_window_glass": "clear", "transparency_window_opacity": "60"}
DEFAULTS = {"transparency": "off", "transparency_windows": "titlebars",
            "transparency_window_glass": "frosted", "transparency_window_opacity": "85"}


def notepads(dbg):
    return sorted([w for w in dbg.windows() if w["title"] == "untitled" and not w.get("popup")],
                  key=lambda w: w["z"])


def set_conf(dbg, **kv):
    for k, v in kv.items():
        dbg.send(f"sh config set desktop.{k} {v}")
    dbg.settle(1.0)


def drag_title(dbg, w, x, y):
    """Moves `w` so its top-left lands at (x, y), by its title bar."""
    gx, gy = w["x"] + 150, w["y"] + 10
    dbg.drag(gx, gy, gx + x - w["x"], gy + y - w["y"], steps=12)
    dbg.settle(1.0)


def by_pid(dbg, pid):
    return next((w for w in notepads(dbg) if w["client_pid"] == pid), None)


def pixel(qmp, tmp, name, x, y):
    from PIL import Image
    p = os.path.join(tmp, f"occlusion_{name}.png")
    qmp.screenshot(p)
    return Image.open(p).convert("RGB").getpixel((x, y))


def run(dbg, qmp, tmp, res):
    for _ in range(2):
        dbg.send("gui spawn /bin/wm/apps/notepad")
        deadline = time.time() + 20
        n = len(notepads(dbg))
        while time.time() < deadline and len(notepads(dbg)) <= n:
            time.sleep(0.2)
    dbg.settle(1.0)
    time.sleep(1.0)
    ws = notepads(dbg)
    if not res.check("two Notepads opened", len(ws) >= 2, str(len(ws))):
        return
    a_pid, b_pid = ws[-2]["client_pid"], ws[-1]["client_pid"]
    sw = dbg.state()["screen"]["w"]
    dbg.damage_verify(True)
    try:
        # --- COVERED ------------------------------------------------------
        # B opened last, so it is on top with its title bar clear of A.
        b = by_pid(dbg, b_pid)
        dbg.rclick(b["x"] + b["w"] // 2, b["y"] + 6)
        dbg.settle()
        row = dbg.ctxmenu_row("Maximize")
        res.check("B's title-bar menu offers Maximize", row is not None)
        if row:
            dbg.click(*row)
            dbg.settle(1.0)
            time.sleep(1.0)
        a, b = by_pid(dbg, a_pid), by_pid(dbg, b_pid)
        res.check("B is maximized, on top, and covers A",
                  b["state"] == "maximized" and b["z"] > a["z"] and
                  b["x"] <= a["x"] - 32 and b["y"] <= max(0, a["y"] - 32),
                  f"A={a['x'], a['y'], a['w'], a['h']} B={b['x'], b['y'], b['w'], b['h']} {b['state']}")
        dbg.move(sw - 4, 4)
        dbg.settle()
        dbg.send("gui compositor reset")
        # B's toolbar row, over A: each hover change is a present whose
        # damage lies on A's rect.
        ty = b["content"]["y"] + 34
        for k in range(16):
            dbg.move(a["x"] + 20 + (k % 8) * 26, ty, settle=False)
            time.sleep(0.12)
        dbg.move(sw - 4, 4)
        dbg.settle(1.0)
        c = dbg.json("gui compositor --json")["windows"]
        res.check("frames whose damage reached A culled it",
                  c["culled_total"] >= 1, str(c))
        res.check("...and A was never drawn: at most one window a frame",
                  c["drawn_total"] <= c["frames"], str(c))

        # --- SEE-THROUGH --------------------------------------------------
        dbg.rclick(b["x"] + b["w"] // 2, b["y"] + 6)
        dbg.settle()
        row = dbg.ctxmenu_row("Restore")
        if row:
            dbg.click(*row)
            dbg.settle(1.0)
            time.sleep(0.5)
        # B (on top) to its place first, with A still where it opened --
        # away from the probe P, which is where A's title bar will go.
        ax, ay = 500, 300
        aw = by_pid(dbg, a_pid)["w"]
        px, py = ax + aw // 2 - 60, ay + 8
        drag_title(dbg, by_pid(dbg, b_pid), px - 300, py - 200)
        dbg.move(sw - 4, 4)
        dbg.settle(1.0)
        off = pixel(qmp, tmp, "off", px, py)
        res.check("transparency off: the probe is B's white body", off == (255, 255, 255), str(off))
        set_conf(dbg, **GLASS)
        no_a = pixel(qmp, tmp, "glass_no_a", px, py)
        drag_title(dbg, by_pid(dbg, a_pid), ax, ay)          # raises A...
        b = by_pid(dbg, b_pid)
        dbg.click(b["x"] + 60, b["y"] + 8)                   # ...B back on top
        dbg.move(sw - 4, 4)
        dbg.settle(1.0)
        a, b = by_pid(dbg, a_pid), by_pid(dbg, b_pid)
        under = (a["x"] <= px < a["x"] + a["w"] and a["y"] <= py < a["y"] + 20 and
                 b["x"] <= px < b["x"] + b["w"] and b["y"] <= py < b["y"] + b["h"] and
                 b["z"] > a["z"])
        res.check("A's title bar lies under B's body at the probe", under,
                  f"A={a['x'], a['y']} B={b['x'], b['y']} z {a['z']}<{b['z']}")
        with_a = pixel(qmp, tmp, "glass_with_a", px, py)
        diff = max(abs(p - q) for p, q in zip(with_a, no_a))
        res.check("glass: A shows through B, so A was drawn under it",
                  diff >= 12 and with_a != (255, 255, 255),
                  f"with A {with_a}, without {no_a}, max channel change {diff}")
        bugs = dbg.damage_bugs()
        res.check("the damage verifier (reference render: no culling) saw nothing stale",
                  bugs == [], "; ".join(bugs)[:400])
    finally:
        dbg.damage_verify(False)
        set_conf(dbg, **DEFAULTS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "occlusion_test")
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
    return res.finish("occlusion_test")


if __name__ == "__main__":
    sys.exit(main())
