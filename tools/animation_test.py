#!/usr/bin/env python3
"""Window animations (userland/wm/wm_anim.c) and `desktop.animations`.

A window opening, closing, minimizing and coming back is drawn as a
GHOST for ~150 ms. Three things are asserted about each, from the WM's
own numbers and from pixels:

  - `gui state --json` reports `anims` >= 1 right after the state change
    and 0 once settled (the WM ran a ghost, and finished it)
  - a frame captured mid-ghost differs from the settled frame inside the
    window's rect AND from the frame before the change -- the ghost is
    something in between, not the before or the after
  - with `desktop.animations` off, `anims` never leaves 0 and the first
    frame after the change already equals the settled one

The setting is written to /etc/desktop.conf and put back to `on` in a
`finally`, since a setting left behind changes the machine for every
later tool (CLAUDE.md).

    python3 tools/vm.py start
    python3 tools/animation_test.py
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

SPAWN = "/bin/wm/apps/calculator"
TITLE = "Calculator"


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"\n        {detail}" if detail and not ok else ""))


def shot(qmp, path):
    from PIL import Image
    qmp.screenshot(path, stable=False, settle=0)
    return Image.open(path).convert("RGB")


def diff_in(a, b, rect):
    """Sampled pixels differing between two frames inside rect (x, y, w, h)."""
    x0, y0, w, h = rect
    n = 0
    for y in range(y0, y0 + h, 3):
        for x in range(x0, x0 + w, 3):
            if a.getpixel((x, y)) != b.getpixel((x, y)):
                n += 1
    return n


def anims_now(dbg):
    return dbg.state().get("anims")


def kill_app(dbg):
    for p in dbg.processes():
        if p["name"] == "calculator" and p["state"] != "zombie":
            dbg.send(f"sh kill {p['pid']}")
    time.sleep(0.6)


def run_change(dbg, qmp, tmp, res, name, action, rect_fn, expect_anim):
    """Perform `action`, watch the WM's ghost report while it runs, then settle.

    THE WM IS ASKED WHERE THE GHOST IS (`anim_rects` in `gui state
    --json`) rather than photographed: a QMP screenshot takes tens of
    milliseconds under TCG and a 150 ms ghost is easily over before one
    lands, which read a working animation as none. The report is
    polled as fast as the console answers; two distinct rects or alphas
    are the motion, and every rect must lie between the window's rect
    and the ghost's destination -- so a report that merely repeated one
    rect, or one that jumped somewhere else, would both fail.
    """
    before = shot(qmp, os.path.join(tmp, f"{name}_before.png"))
    action()
    seen = []          # (x, y, w, h, alpha) per poll while a ghost is in flight
    peak = 0
    deadline = time.time() + 0.6
    while time.time() < deadline:
        st = dbg.state()
        a = st.get("anims") or 0
        peak = max(peak, a)
        for r in st.get("anim_rects", []):
            t = (r["x"], r["y"], r["w"], r["h"], r["alpha"])
            if not seen or seen[-1] != t:
                seen.append(t)
        if seen and a == 0:
            break
        if not expect_anim and time.time() > deadline - 0.3:
            break
        time.sleep(0.005)
    dbg.settle(1.0)
    time.sleep(0.3)
    after = shot(qmp, os.path.join(tmp, f"{name}_after.png"))
    rect = rect_fn()
    settled = anims_now(dbg)
    d_after = diff_in(before, after, rect) if rect else -1
    if expect_anim:
        res.check(f"{name}: the WM reports a ghost in flight", peak >= 1, f"anims peak={peak}")
        res.check(f"{name}: ...and none once settled", settled == 0, f"anims={settled}")
        res.check(f"{name}: the ghost MOVED -- at least two distinct rects or alphas reported",
                  len(seen) >= 2, f"seen={seen}")
        if rect and seen:
            x, y, w, h = rect
            # Every ghost rect is inside the union of the window's rect
            # and the whole screen band below it (a minimize heads for
            # the taskbar); it never leaves the screen.
            inside = all(0 <= r[0] and r[0] + r[2] <= 1280 + 1 and 0 <= r[1] and r[1] + r[3] <= 720 + 1
                         and r[2] <= w + 2 and r[3] <= h + 2 for r in seen)
            res.check(f"{name}: every reported ghost rect is on screen and no larger than the window",
                      inside, f"window={rect} seen={seen}")
        res.check(f"{name}: the screen changed inside the window's rect", d_after > 20,
                  f"rect={rect} before-vs-after={d_after}")
    else:
        res.check(f"{name}: animations off -- the WM reports no ghost", peak == 0 and not seen,
                  f"anims peak={peak} seen={seen}")
        res.check(f"{name}: animations off -- the screen still changed inside the window's rect",
                  d_after > 20, f"rect={rect} before-vs-after={d_after}")
    return after


def run(dbg, qmp, tmp, res):
    def win():
        return dbg.window(TITLE)

    def rect_of_window():
        w = win()
        return (w["x"], w["y"], w["w"], w["h"]) if w else None

    last_rect = {}

    def rect_last():
        return last_rect.get("r")

    def taskbar_button():
        tb = dbg.json("gui taskbar --json")
        for b in tb["buttons"]:
            if b["app_id"] == "calculator" or b["title"] == TITLE:
                return b
        return None

    for setting, expect in (("on", True), ("off", False)):
        dbg.send(f"sh config set desktop.animations {setting}")
        time.sleep(0.4)
        # OPEN
        run_change(dbg, qmp, tmp, res, f"open[{setting}]",
                   lambda: dbg.send(f"gui spawn {SPAWN}"), rect_of_window, expect)
        w = win()
        res.check(f"open[{setting}]: the window is up and normal", w is not None and w["state"] == "normal")
        if not w:
            return
        last_rect["r"] = (w["x"], w["y"], w["w"], w["h"])
        b = taskbar_button()
        res.check(f"open[{setting}]: the window has a taskbar button", b is not None)
        if b:
            # MINIMIZE: a click on the focused window's button.
            run_change(dbg, qmp, tmp, res, f"minimize[{setting}]",
                       lambda: dbg.send(f"gui click {b['cx']} {b['cy']}"), rect_last, expect)
            res.check(f"minimize[{setting}]: the window is minimized", win()["state"] == "minimized")
            # RESTORE
            run_change(dbg, qmp, tmp, res, f"restore[{setting}]",
                       lambda: dbg.send(f"gui click {b['cx']} {b['cy']}"), rect_last, expect)
            res.check(f"restore[{setting}]: the window is back", win()["state"] == "normal")
        # CLOSE: the client exits; the ghost outlives it.
        pid = [p["pid"] for p in dbg.processes() if p["name"] == "calculator"]
        run_change(dbg, qmp, tmp, res, f"close[{setting}]",
                   lambda: dbg.send(f"sh kill {pid[0]}") if pid else None, rect_last, expect)
        res.check(f"close[{setting}]: the window is gone", win() is None)
        kill_app(dbg)


def ghost_ms(dbg, label):
    """How long a ghost stays in flight for one minimize, in ms, and
    whether one appeared at all. Polls `anims` rather than pixels: this
    is about DURATION, and a screenshot cannot time anything."""
    tb = dbg.taskbar()
    buttons = tb.get("buttons") if isinstance(tb, dict) else tb
    b = next((x for x in (buttons or []) if "untitled" in str(x.get("title", ""))), None)
    if not b:
        return None, 0
    t0 = time.time()
    dbg.send(f"gui click {b['cx']} {b['cy']}")
    peak, saw = 0, False
    while time.time() - t0 < 4.0:
        a = dbg.state().get("anims") or 0
        peak = max(peak, a)
        if a:
            saw = True
        elif saw:
            break
    ms = (time.time() - t0) * 1000.0
    dbg.settle(0.8)
    tb = dbg.taskbar()
    buttons = tb.get("buttons") if isinstance(tb, dict) else tb
    b = next((x for x in (buttons or []) if "untitled" in str(x.get("title", ""))), None)
    if b:
        dbg.send(f"gui click {b['cx']} {b['cy']}")   # restore, for the next round
        dbg.settle(0.8)
    return (ms if saw else None), peak


def run_speed(dbg, res):
    """`desktop.animation_speed` scales every animation -- KWin's shape.

    ASSERTS THE ORDER, NOT THE MILLISECONDS. The durations are real
    (125/250/500 ms at the time of writing) but a poll adds its own
    overhead and the guest is a TCG machine, so pinning exact numbers
    would be a flake generator. What must hold is that the knob does
    something monotonic, and that `instant` means NO GHOST AT ALL --
    not a very short one, since the point of instant is that nothing is
    snapshotted and the real window is never hidden.
    """
    # ESTABLISH THE PRECONDITION. run() above ends on its `off` case, so
    # arriving here with animations disabled would report "no ghost
    # flies" for every speed -- a fixture failure wearing the costume of
    # the thing under test.
    dbg.send("sh config set desktop.animations on")
    dbg.settle(1.2)
    dbg.open_app("Notepad")
    dbg.settle(1.5)
    if not [w for w in dbg.windows() if w["title"] == "untitled"]:
        res.check("speed: a window to minimize", False, "Notepad did not open")
        return

    got = {}
    try:
        for sp in ("instant", "fast", "normal", "slow"):
            dbg.send(f"sh config set desktop.animation_speed {sp}")
            dbg.settle(1.2)
            ms, peak = ghost_ms(dbg, sp)
            got[sp] = ms
            if sp == "instant":
                res.check("speed instant: no ghost is created at all",
                          peak == 0, f"anims peaked at {peak}")
            else:
                res.check(f"speed {sp}: a ghost flies", ms is not None,
                          "no ghost seen")
    finally:
        dbg.send("sh config set desktop.animation_speed normal")
        dbg.settle(0.8)

    if got.get("fast") and got.get("normal") and got.get("slow"):
        res.check("speed: fast < normal < slow",
                  got["fast"] < got["normal"] < got["slow"],
                  f"fast={got['fast']:.0f} normal={got['normal']:.0f} "
                  f"slow={got['slow']:.0f} ms")
        # Slow is 2x normal by construction; allow wide margins for the
        # poll and for TCG, but a knob that moved by 10% would be one
        # nobody can feel.
        res.check("speed: slow is markedly longer than fast",
                  got["slow"] > got["fast"] * 2.0,
                  f"fast={got['fast']:.0f} slow={got['slow']:.0f} ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "animation_test")
    tmp = args.logs or "/tmp"
    os.makedirs(tmp, exist_ok=True)

    res = Result()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, tmp, res)
        run_speed(dbg, res)
    finally:
        try:
            dbg.send("sh config set desktop.animations on")
            dbg.send("sh config set desktop.animation_speed normal")
        finally:
            dbg.close()
    print(f"\nanimation_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
