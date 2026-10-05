#!/usr/bin/env python3
"""tools/taskbar_style_test.py -- the taskbar's independent layout
settings and its two themes, each checked where it is DRAWN as well as
where it is reported.

WHAT THIS IS
------------
`desktop.taskbar_buttons` labelled | icons, `desktop.taskbar_align` and
`desktop.start_position` left | center, `desktop.taskbar_float` off | on
and `desktop.taskbar_theme` dark | light (userland/wm/wm_taskbar.h). One
layout function and one draw function serve all of them, reading
taskbar_geom(), so each check pairs the WM's report (`gui taskbar
--json`, from that same function) with a pixel the report could not
fake:

  * the defaults -- labelled buttons from the left, the panel filling
    the band, the Start button icon-only and owning the corner.
  * floating: the band grows by the gap and the panel stands off the
    edges -- the screen's corner pixel is NOT the strip's colour -- and
    it DEFLATES to fill the band while a window is maximized.
  * icons + centre + Start centre (Windows 11): 48px icon buttons
    centred with Start; hovering one lights it and (with peek off) arms
    the tooltip with the full title. Start left instead puts it at x=0
    and centres the rest; that floats too. Icons from the LEFT pack
    straight after Start; labelled buttons can be centred; and a
    centred Start holds the buttons centred -- the alignment's
    Requires= refuses `left` and reads `center` until Start returns.
  * light: the strip is drawn in the light palette.
  * the group count: an icons-only group carries a badge with its count,
    in the strip's ink, while its window has the focus; a single
    window has none; a labelled group reads "Notepad (2)".

    python3 tools/vm.py --disk <copy> start
    python3 tools/taskbar_style_test.py
    echo $?                            # 0 = every check passed

POSITIVE CONTROL
----------------
Making taskbar_floating() return 0 turns the floating checks red (the
corner pixel stays the strip's colour, `floating` reports false), and
the deflate check with them; dropping the `hovered` fill in
draw_taskbar() turns "the hovered button is drawn lit" red while the
JSON hover check stays green -- the reason both exist. No badge
(taskbar_badge_rect() returning 0) reddens the badge check alone, and
naming a group by its app id reddens the label check alone.

It leaves every taskbar setting UNSET on the way out: `make iso` syncs
rather than reformats, so a setting written here outlives the run.
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results  # noqa: E402


Result = Results


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def taskbar(dbg):
    return dbg.json("gui taskbar --json")


def set_and_wait(dbg, name, value, pred, timeout=8.0):
    """Write a setting and wait for the WM to ADOPT it -- the report
    says so -- rather than sleeping a guess."""
    if value is None:
        dbg.send(f"sh config unset {name}")
    else:
        dbg.send(f"sh config set {name} {value}")
    deadline = time.time() + timeout
    tb = None
    while time.time() < deadline:
        tb = taskbar(dbg)
        if tb and pred(tb):
            break
        time.sleep(0.4)
    dbg.settle()
    return taskbar(dbg)


def pixel(qmp, tmp, name, x, y):
    from PIL import Image
    p = os.path.join(tmp, name + ".png")
    qmp.screenshot(p)
    return Image.open(p).convert("RGB").getpixel((x, y))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "taskbar_style_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="taskbar_style_")
    res = Result()

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        # A clean fixture: nothing a previous tool left behind.
        for k in ("taskbar_buttons", "taskbar_align", "taskbar_float", "start_position", "taskbar_theme",
                  "taskbar_height", "start_button", "taskbar_peek", "taskbar_combine"):
            dbg.send(f"sh config unset desktop.{k}")
        dbg.settle()
        for app in ("Terminal", "Notepad"):
            dbg.open_app(app)
        deadline = time.time() + 20
        while time.time() < deadline and len(taskbar(dbg).get("buttons", [])) < 2:
            time.sleep(0.5)
        dbg.settle()

        # --- classic, the default ------------------------------------
        print("classic (the default)")
        tb = set_and_wait(dbg, "desktop.taskbar_buttons", None,
                          lambda t: t.get("buttons_kind") == "labelled")
        sh = tb["y"] + tb["h"]
        res.check("the defaults are labelled, left, Start left, dark, 48px",
                  tb.get("buttons_kind") == "labelled" and tb.get("align_center") is False
                  and tb.get("start_center") is False and tb.get("theme") == "dark" and tb["h"] == 48,
                  f"{ {k: tb.get(k) for k in ('buttons_kind', 'align_center', 'start_center', 'theme', 'h')} }")
        p = tb["panel"]
        res.check("classic's panel fills the band",
                  p["x"] == 0 and p["y"] == tb["y"] and p["h"] == tb["h"] and p["r"] == 0, f"{p}")
        st = tb["start"]
        res.check("Start is icon-only by default and owns the screen's corner",
                  st.get("mark") is not None and st["x"] == 0, f"start: {st}")
        btns = tb["buttons"]
        res.check("the window buttons are labelled with their titles",
                  len(btns) >= 2 and all(b["label"] == b["title"] for b in btns),
                  f"{[(b['title'], b['label']) for b in btns]}")
        corner = pixel(qmp, tmp, "classic", 2, tb["y"] + tb["h"] // 2)
        res.check("the screen's bottom-left corner is the strip",
                  corner == rgb(tb["bar"]), f"{corner} vs bar {rgb(tb['bar'])}")

        # --- floating ------------------------------------------------
        print("floating")
        tb = set_and_wait(dbg, "desktop.taskbar_float", "on",
                          lambda t: t.get("float") is True)
        p = tb["panel"]
        res.check("the band grows by the gap and the panel stands off the edges",
                  tb.get("floating") is True and tb["h"] == 56 and tb["y"] + tb["h"] == sh
                  and p["x"] == 8 and p["w"] == dbg.state()["screen"]["w"] - 16 and p["r"] > 0,
                  f"floating={tb.get('floating')} h={tb['h']} panel={p}")
        # Inside the gap under the panel -- from the SCREEN's edge, not
        # the reported panel, so a wrong report cannot move the probe.
        gap_y = sh - 4
        corner = pixel(qmp, tmp, "floating", 2, gap_y)
        inside = pixel(qmp, tmp, "floating2", p["x"] + p["w"] // 2, p["y"] + 2)
        res.check("the gap is not painted as strip; the panel is",
                  corner != rgb(tb["bar"]) and inside == rgb(tb["bar"]),
                  f"gap {corner}, panel {inside}, bar {rgb(tb['bar'])}")

        # The TOPMOST window: its title bar is the one nothing can cover.
        wins = dbg.windows()
        win = wins[-1] if wins else None
        maximized = False
        if win:
            dbg.rclick(win["x"] + win["w"] // 2, win["y"] + 6)
            dbg.settle()
            row = dbg.ctxmenu_row("Maximize")
            if row:
                dbg.click(*row)
                maximized = True
        deadline = time.time() + 8
        while time.time() < deadline and taskbar(dbg).get("floating"):
            time.sleep(0.4)
        dbg.settle()
        tb = taskbar(dbg)
        p = tb["panel"]
        res.check("a maximized window deflates the panel to fill the band",
                  maximized and tb.get("floating") is False and p["x"] == 0
                  and p["h"] == tb["h"] == 56,
                  f"maximized={maximized} floating={tb.get('floating')} panel={p}")
        corner = pixel(qmp, tmp, "deflated", 2, gap_y)
        res.check("...and is drawn so, down to the corner",
                  corner == rgb(tb["bar"]), f"{corner} vs bar {rgb(tb['bar'])}")
        wins = dbg.windows()
        win = wins[-1] if wins else None
        if win:
            dbg.rclick(win["x"] + win["w"] // 2, win["y"] + 6)
            dbg.settle()
            row = dbg.ctxmenu_row("Restore")
            if row:
                dbg.click(*row)
        dbg.settle()

        # --- icons, centred with Start (Windows 11) ---------------------
        print("icons, centred with Start")
        set_and_wait(dbg, "desktop.taskbar_float", None, lambda t: t.get("float") is False)
        # The tooltip half below is the peek-OFF behaviour; peek has its
        # own tool (taskbar_peek_test.py).
        dbg.send("sh config set desktop.taskbar_peek off")
        dbg.send("sh config set desktop.taskbar_buttons icons")
        dbg.send("sh config set desktop.taskbar_align center")
        tb = set_and_wait(dbg, "desktop.start_position", "center",
                          lambda t: t.get("buttons_kind") == "icons" and t.get("align_center")
                          and t.get("start_center"))
        btns = tb["buttons"]
        sw = dbg.state()["screen"]["w"]
        square = all(b["w"] == tb["btn_h"] + 8 for b in btns)
        left = tb["start"]["x"]
        right = max(b["x"] + b["w"] for b in btns) if btns else 0
        res.check("the buttons are square icons, centred with Start",
                  btns and square and abs((left + right) / 2 - sw / 2) <= 3,
                  f"widths {[b['w'] for b in btns]} btn_h {tb['btn_h']}, group {left}..{right} on {sw}")

        # HOVER: a button that is NOT focused (selection outranks hover),
        # sampled at its left edge, away from the cursor sprite.
        focus = [w for w in dbg.windows() if w.get("focused")]
        target = next((b for b in btns if b["title"] != (focus[0]["title"] if focus else None)), None)
        if target:
            sx, sy = target["x"] + 3, tb["btn_y"] + tb["btn_h"] // 2
            dbg.warp_cursor(qmp, 200, 200)
            rest = pixel(qmp, tmp, "rest", sx, sy)
            dbg.warp_cursor(qmp, target["cx"], tb["btn_y"] + tb["btn_h"] // 2)
            time.sleep(0.3)
            hov = taskbar(dbg).get("hover")
            lit = pixel(qmp, tmp, "hover", sx, sy)
            res.check("the WM reports the button under the pointer as hovered",
                      hov == target["index"], f"hover={hov}, button index {target['index']}")
            res.check("the hovered button is drawn lit",
                      rest == rgb(tb["bar"]) and lit != rest, f"rest {rest}, hovered {lit}")
            deadline = time.time() + 4
            tip = None
            while time.time() < deadline:
                tip = dbg.json("gui tooltip --json")
                if tip and tip.get("text"):
                    break
                time.sleep(0.3)
            res.check("an icon-only button's tooltip names its window",
                      bool(tip) and tip.get("text") == target["title"], f"tooltip: {tip}")
            dbg.warp_cursor(qmp, 200, 200)
        else:
            res.check("a non-focused button to hover", False, f"{btns}")

        # START LEFT: x=0, and the buttons still centred on their own,
        # clear of it.
        tb = set_and_wait(dbg, "desktop.start_position", None,
                          lambda t: t.get("start_center") is False)
        btns = tb["buttons"]
        left = btns[0]["x"] if btns else 0
        right = max(b["x"] + b["w"] for b in btns) if btns else 0
        res.check("Start left puts it in the corner and the icons stay centred",
                  tb["start"]["x"] == 0 and abs((left + right) / 2 - sw / 2) <= 3
                  and left > tb["start"]["w"],
                  f"start {tb['start']}, buttons {left}..{right}")
        # AND IT FLOATS: the two settings are independent.
        tb = set_and_wait(dbg, "desktop.taskbar_float", "on",
                          lambda t: t.get("floating") is True)
        corner = pixel(qmp, tmp, "centred-float", 2, sh - 4)
        res.check("the centred icons float too",
                  tb["buttons_kind"] == "icons" and tb["panel"]["x"] == 8 and corner != rgb(tb["bar"]),
                  f"buttons {tb['buttons_kind']} panel {tb['panel']} corner {corner}")
        set_and_wait(dbg, "desktop.taskbar_float", None, lambda t: t.get("float") is False)

        # ICONS FROM THE LEFT (Plasma's default): packed straight after Start.
        tb = set_and_wait(dbg, "desktop.taskbar_align", None,
                          lambda t: t.get("align_center") is False)
        btns = tb["buttons"]
        packed = all(btns[k + 1]["x"] - btns[k]["x"] == btns[k]["w"] + 4 for k in range(len(btns) - 1))
        res.check("icons from the left pack straight after Start",
                  btns and btns[0]["x"] == tb["start"]["x"] + tb["start"]["w"] + 4 and packed,
                  f"start {tb['start']}, buttons {[(b['x'], b['w']) for b in btns]}")

        # A CENTRED START CENTRES THE BUTTONS. The alignment's declaration
        # Requires= a left Start, so meanwhile it READS `center` (its
        # Otherwise=), a write of `left` is refused with the reason, and
        # the stored value comes back when Start does.
        tb = set_and_wait(dbg, "desktop.start_position", "center",
                          lambda t: t.get("start_center") is True)
        reply = dbg.send("sh config set desktop.taskbar_align left") or ""
        time.sleep(1.0)
        tb = taskbar(dbg)
        got = dbg.send("sh config get desktop.taskbar_align") or ""
        res.check("a centred Start keeps the buttons centred and refuses 'left'",
                  tb.get("align_center") is True and "Start button" in reply and "center" in got,
                  f"align_center={tb.get('align_center')} set->{reply.strip()!r} get->{got.strip()!r}")
        tb = set_and_wait(dbg, "desktop.start_position", None,
                          lambda t: t.get("start_center") is False)
        res.check("...and Start back on the left brings the stored alignment back",
                  tb.get("align_center") is False, f"align_center={tb.get('align_center')}")

        # LABELLED AND CENTRED: the labels survive, the group centres.
        dbg.send("sh config unset desktop.taskbar_buttons")
        tb = set_and_wait(dbg, "desktop.taskbar_align", "center",
                          lambda t: t.get("align_center") and t.get("buttons_kind") == "labelled")
        btns = tb["buttons"]
        left = btns[0]["x"] if btns else 0
        right = max(b["x"] + b["w"] for b in btns) if btns else 0
        res.check("labelled buttons can be centred too",
                  btns and all(b["label"] == b["title"] for b in btns)
                  and abs((left + right) / 2 - sw / 2) <= 3,
                  f"buttons {left}..{right} on {sw}, labels {[b['label'] for b in btns]}")
        set_and_wait(dbg, "desktop.taskbar_align", None, lambda t: t.get("align_center") is False)
        dbg.send("sh config unset desktop.taskbar_peek")

        # --- the light theme -------------------------------------------
        print("light")
        tb = set_and_wait(dbg, "desktop.taskbar_theme", "light",
                          lambda t: t.get("theme") == "light")
        spot = pixel(qmp, tmp, "light", tb["tray_x"] - 10, tb["btn_y"] + 2)
        res.check("the light theme draws the light strip",
                  tb["bar"] == 0xECECEC and spot == (236, 236, 236), f"bar {tb['bar']:06x}, pixel {spot}")

        # Back to the defaults for every later tool.
        set_and_wait(dbg, "desktop.taskbar_theme", None, lambda t: t.get("theme") == "dark")

        # --- the group count ---------------------------------------------
        #
        # An icons-only GROUP says how many windows it holds in a badge;
        # a single window has none. The second Notepad takes the focus,
        # which is the case the old doubled pill went missing in.
        print("the group count")
        dbg.open_app("Notepad")
        set_and_wait(dbg, "desktop.taskbar_combine", "always", lambda t: t.get("combine") == "always")
        tb = set_and_wait(dbg, "desktop.taskbar_buttons", "icons",
                          lambda t: t.get("buttons_kind") == "icons"
                          and any(b["count"] == 2 for b in t.get("buttons", [])))
        grp = next((b for b in tb["buttons"] if b["count"] == 2), None)
        one = next((b for b in tb["buttons"] if b["count"] == 1), None)
        res.check("an icons-only group carries a count badge and a single window none",
                  grp is not None and grp.get("badge") and grp["badge"]["text"] == "2"
                  and one is not None and one.get("badge") is None,
                  f"group {grp and grp.get('badge')}, single {one and one.get('badge')}")
        if grp and grp.get("badge") and one:
            bd = grp["badge"]
            # Inside the capsule, left of the digit: the strip's ink. The
            # same offset on the single button is icon, not badge.
            spot = (bd["x"] + 2, bd["y"] + bd["h"] // 2)
            off = (one["x"] + (bd["x"] - grp["x"]) + 2, spot[1])
            from PIL import Image
            path = os.path.join(tmp, "badge.png")
            qmp.screenshot(path)
            im = Image.open(path).convert("RGB")
            res.check("...drawn in the strip's ink, while its window has the focus",
                      im.getpixel(spot) == rgb(tb["ink"]) and im.getpixel(off) != rgb(tb["ink"]),
                      f"badge {im.getpixel(spot)}, single {im.getpixel(off)}, ink {rgb(tb['ink'])}")
        tb = set_and_wait(dbg, "desktop.taskbar_buttons", None,
                          lambda t: t.get("buttons_kind") == "labelled")
        grp = next((b for b in tb["buttons"] if b["count"] == 2), None)
        res.check("a labelled group is named after the app, with its count",
                  grp is not None and grp["label"] == "Notepad (2)" and grp.get("badge") is None,
                  f"label {grp and grp['label']!r}, badge {grp and grp.get('badge')}")

        for k in ("taskbar_buttons", "taskbar_align", "start_position", "taskbar_float",
                  "taskbar_combine"):
            dbg.send(f"sh config unset desktop.{k}")

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\ntaskbar_style_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
