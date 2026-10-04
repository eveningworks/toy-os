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

And two about what is damaged rather than drawn:

  - SUBMENU SHADOW: the desktop menu's Icon size submenu, opened and then
    closed by hover, leaves no shadow band beyond its far edge (the
    control: the band IS darker while it is open).
  - CLOSED BY ANOTHER: the calendar, closed by opening the network
    flyout, leaves its rect wallpaper again -- the core damages a closed
    overlay's last rect itself.
  - CLOSED BY A CLICK OUTSIDE: the network (and, where the guest shows
    it, the remote) flyout, dismissed with the pointer not moving, is
    wallpaper again -- by pixels, with the verifier off, since a close
    that asks for no frame leaves the verifier nothing to judge.
  - UNCHANGED PRESENT: re-setting `system.timezone` to its own value makes
    every client re-present an identical frame; each must be counted as
    unchanged, and no present may ask for a full frame (`presents_full`).

    python3 tools/vm.py start
    python3 tools/occlusion_test.py
    python3 tools/vm.py stop

Positive controls (tools/mutate.py): disabling the cull reddens COVERED;
counting a see-through window as opaque reddens the verifier; damaging a
closed submenu without its shadow margin reddens SUBMENU SHADOW; turning
an empty damage list back into a frame reddens UNCHANGED PRESENT.
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


def shot(qmp, tmp, name):
    from PIL import Image
    p = os.path.join(tmp, f"occlusion_{name}.png")
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def submenu_shadow(dbg, qmp, tmp, res):
    """A submenu that closes takes its SHADOW with it: the desktop's menu,
    Icon size > opened by hovering, closed by hovering Refresh. The pixel
    just outside the submenu's far edge must be darker while it is open
    (the control: there is a shadow to leave behind) and the wallpaper's
    own again once it closes."""
    st = dbg.state()["screen"]
    x, y = st["w"] // 3, st["h"] // 4
    dbg.warp_cursor(qmp, x, y)
    dbg.settle()
    before = shot(qmp, tmp, "sub_before")
    dbg.damage_verify(True)
    dbg.damage_bugs()   # drop anything older
    dbg.rclick(x, y)
    dbg.settle()
    row = dbg.ctxmenu_row("Icon size")
    if not res.check("the desktop menu offers Icon size", row is not None):
        dbg.key("0x1b")
        return
    dbg.warp_cursor(qmp, *row)
    dbg.settle(1.0)
    m = dbg.ctxmenu()
    sub = m.get("sub")
    if not res.check("hovering Icon size opens its submenu", sub is not None, str(m)[:200]):
        dbg.key("0x1b")
        return
    right = sub["x"] > m["x"]
    q = (sub["x"] + sub["w"] + 4 if right else sub["x"] - 5, sub["y"] + 12)
    opened = shot(qmp, tmp, "sub_open").getpixel(q)
    refresh = dbg.ctxmenu_row("Refresh")
    if not res.check("the desktop menu offers Refresh", refresh is not None):
        dbg.key("0x1b")
        return
    dbg.warp_cursor(qmp, *refresh)
    dbg.settle(1.0)
    closed_ok = dbg.ctxmenu().get("sub") is None
    after = shot(qmp, tmp, "sub_closed").getpixel(q)
    was = before.getpixel(q)
    res.check("the open submenu casts a shadow at the probe (the control)",
              sum(was) - sum(opened) >= 12, f"before {was}, open {opened} at {q}")
    res.check("...and once it closes the shadow is gone",
              closed_ok and max(abs(p - r) for p, r in zip(after, was)) <= 1,
              f"before {was}, after {after}, submenu closed={closed_ok}")
    dbg.key("0x1b")
    dbg.settle()
    # THE PIXEL CAN BE HEALED by any later full frame (a debug command
    # asks for some), so the verifier -- which judges the frame the
    # submenu closed in -- is the check that cannot be.
    bugs = dbg.damage_bugs()
    res.check("...and the verifier saw no stale shadow while it closed", bugs == [],
              "; ".join(bugs)[:400])


def calendar_closed_by_flyout(dbg, qmp, tmp, res):
    """An overlay closed by ANOTHER opening is repainted where it was --
    the core damages a closed overlay's last rect, whoever closed it.
    Open the calendar from the tray clock, then the network flyout (which
    closes it), and sample the calendar's former rect away from the
    flyout: the wallpaper's own pixels again, where while it was open they
    were the panel's (the control)."""
    g = dbg.json("gui calendar --json")
    n = dbg.json("gui network --json")
    if not res.check("the tray has a clock and a network item to click",
                     g.get("clock") and n.get("tray", {}).get("w", 0) > 0, str(n.get("tray"))):
        return
    sw = dbg.state()["screen"]["w"]
    dbg.warp_cursor(qmp, sw // 2, 40)
    dbg.settle()
    before = shot(qmp, tmp, "cal_before")
    dbg.send(f"gui click {g['clock']['cx']} {g['clock']['cy']}")
    dbg.settle(1.0)
    g = dbg.json("gui calendar --json")
    if not res.check("clicking the clock opened the calendar", g.get("open")):
        return
    opened = shot(qmp, tmp, "cal_open")
    dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}")
    dbg.settle(1.0)
    n = dbg.json("gui network --json")
    shut = not dbg.json("gui calendar --json").get("open")
    res.check("opening the network flyout closed the calendar", shut and n.get("open"),
              f"calendar closed={shut} network open={n.get('open')}")
    dbg.warp_cursor(qmp, sw // 2, 40)   # no tooltip from the tray icon over the probes
    dbg.settle(1.0)
    after = shot(qmp, tmp, "cal_closed")
    m = 40   # clear of the flyout and its shadow
    pts = [(x, y) for x in range(g["x"] + 8, g["x"] + g["w"] - 8, 16)
           for y in range(g["y"] + 8, g["y"] + g["h"] - 8, 16)
           if not (n["x"] - m <= x < n["x"] + n["w"] + m and n["y"] - m <= y < n["y"] + n["h"] + m)]
    differ_open = sum(1 for p in pts if opened.getpixel(p) != before.getpixel(p))
    stale = [p for p in pts if max(abs(a - b) for a, b in
                                   zip(after.getpixel(p), before.getpixel(p))) > 1]
    res.check("the open calendar covered the probes (the control)",
              pts and differ_open >= len(pts) // 2, f"{differ_open} of {len(pts)} differed")
    res.check("...and once the flyout closed it, its rect is wallpaper again",
              pts and not stale, f"{len(stale)} of {len(pts)} stale, first {stale[:1]}")
    dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}")   # close the flyout
    dbg.settle()
    bugs = dbg.damage_bugs()
    res.check("...and the verifier saw nothing stale as it closed", bugs == [],
              "; ".join(bugs)[:400])


# How soon after a click outside the flyout's close must be on screen. The
# tray clock repaints once a second, so a close that asked for no frame is
# drawn by the next tick -- ~1 s after the click, given it lands just after
# one. Under this bound, over it.
CLOSE_FRAME_S = 0.5


def flyout_closed_by_click(dbg, qmp, tmp, res, name):
    """A tray flyout dismissed by a click OUTSIDE it, with the pointer not
    moving, is gone from the screen AT ONCE: its own close path neither
    damages nor asks for a frame -- the core does both. Pixels, not the
    verifier, which judges nothing when no frame is drawn at all.

    THE TRAY CLOCK DRAWS A FRAME EVERY SECOND and the core damages a
    closed overlay in whatever frame comes next, so a close that asked for
    no frame still clears within a second, and a settled picture cannot
    tell the two apart. So the check that matters is the TIMING: the click
    lands just after a clock tick, and a scene repaint must follow within
    CLOSE_FRAME_S -- well inside the second the next tick needs, and slack
    enough for TCG -- three closes running."""
    j = dbg.json(f"gui {name} --json")
    t = j.get("tray") or {}
    if not t.get("w") or j.get("tray_hidden"):
        print(f"  NOT COVERED  {name}: no tray item on this guest")
        return
    sw = dbg.state()["screen"]["w"]
    park = (sw // 2, 40)                     # empty desktop, far from the tray
    dbg.warp_cursor(qmp, *park)
    dbg.settle(1.0)
    before = shot(qmp, tmp, f"{name}_before")
    prompt = []
    for k in range(3):
        dbg.send(f"gui click {t['cx']} {t['cy']}")
        dbg.settle(1.0)
        j = dbg.json(f"gui {name} --json")
        if not res.check(f"{name}: clicking its tray item opened the flyout (#{k + 1})",
                         j.get("open")):
            return
        if k == 0:
            opened = shot(qmp, tmp, f"{name}_open")
        dbg.warp_cursor(qmp, *park)          # the real pointer, back where it rests
        dbg.settle(1.0)
        # A REAL button press where the pointer already is, not `gui
        # click`: nothing about it but the close may ask for a frame.
        # Click just AFTER a clock tick, so the next one is ~1 s away and
        # cannot stand in for a frame the close failed to ask for.
        dbg.wait_scene_repaint(dbg.scene_repaints(), 1.5)
        s0 = dbg.scene_repaints()
        qmp.click(settle=0.02)
        took = dbg.wait_scene_repaint(s0, CLOSE_FRAME_S)
        prompt.append(round(took, 3) if took is not None else None)
        dbg.settle(1.0)
    shut = not dbg.json(f"gui {name} --json").get("open")
    res.check(f"{name}: every close drew a frame at once, not at the next clock tick",
              all(p is not None for p in prompt),
              f"seconds to the next scene repaint (bound {CLOSE_FRAME_S}): {prompt}")
    after = shot(qmp, tmp, f"{name}_closed")
    pts = [(x, y) for x in range(j["x"] + 6, j["x"] + j["w"] - 6, 14)
           for y in range(j["y"] + 6, j["y"] + j["h"] - 6, 14)]
    differ = sum(1 for p in pts if opened.getpixel(p) != before.getpixel(p))
    stale = [p for p in pts if max(abs(a - b) for a, b in
                                   zip(after.getpixel(p), before.getpixel(p))) > 1]
    res.check(f"{name}: the open flyout covered the probes (the control)",
              pts and differ >= len(pts) // 2, f"{differ} of {len(pts)} differed")
    res.check(f"{name}: a click outside closed it and its rect is wallpaper again",
              shut and pts and not stale,
              f"closed={shut}, {len(stale)} of {len(pts)} stale, first {stale[:1]}")


def unchanged_present(dbg, res):
    """A present that changed nothing draws NO frame. Every client re-presents
    on a setting notice (uapp redraws on WIN_EV_SETTING), almost always an
    identical frame; with the damage list empty, the compositor must not
    turn it into a full-screen repaint. Judged by the compositor's own
    attribution (`presents_full`: presents that asked for a frame while
    damaging nothing of their own -- the only way a present makes a frame
    full-screen), not by the global full-frame count, which a debug
    command may move for its own reasons."""
    def counts():
        c = dbg.json("gui compositor --json")["windows"]
        return c["presents_full"], c["presents_unchanged"]
    tz = (dbg.send("sh config get system.timezone") or "").strip().splitlines()
    tz = tz[-1].split()[-1] if tz else "UTC"
    dbg.settle(1.0)
    full0, same0 = counts()
    for _ in range(3):
        dbg.send(f"sh config set system.timezone {tz}")
        dbg.settle(1.0)
    full1, same1 = counts()
    res.check("an unchanged re-present is recognised as unchanged",
              same1 - same0 >= 2, f"presents_unchanged {same0} -> {same1}")
    res.check("...and no present asked for a full-screen frame",
              full1 == full0, f"presents_full {full0} -> {full1}")


def run(dbg, qmp, tmp, res):
    try:
        submenu_shadow(dbg, qmp, tmp, res)   # turns the verifier on
        calendar_closed_by_flyout(dbg, qmp, tmp, res)
    finally:
        dbg.damage_verify(False)
    for name in ("network", "remote"):   # the verifier OFF: it repairs what it sees
        flyout_closed_by_click(dbg, qmp, tmp, res, name)
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
    unchanged_present(dbg, res)   # before the verifier: it hides frame counts
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
