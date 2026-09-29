#!/usr/bin/env python3
"""tools/taskbar_peek_test.py -- the taskbar's window preview
(`desktop.taskbar_peek`, userland/wm/wm_peek.h): it opens, it shows the
window, it follows the pointer, it acts, and it can be switched off.

WHAT THIS ASSERTS THAT "IT OPENS" WOULD NOT
------------------------------------------
* THE THUMBNAIL IS A PICTURE. The card's thumbnail rect (`gui peek
  --json`, from the same code that draws it) must be neither near-black
  nor one flat colour. The first build drew every thumbnail BLACK --
  uimg_scale() weighted colour by an alpha byte a client buffer leaves
  at zero -- while opening, placing and titling the card perfectly.
* IT SWITCHES without the delay once open, and CLOSES after a short
  grace when the pointer goes to empty desktop.
* A CLICK ON AN ENTRY ACTIVATES that window; its x CLOSES it.
* HIGHLIGHT dims the desktop -- a wallpaper pixel darkens -- while a
  pixel inside the lifted window keeps its value.
* OFF opens nothing.

    python3 tools/vm.py --disk <copy> start
    python3 tools/taskbar_peek_test.py
    echo $?

POSITIVE CONTROL: the black-thumbnail build above reddens "the
thumbnail is a picture" and nothing else. Leaves every setting unset.
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


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


def wait_for(fn, timeout=4.0, step=0.2):
    deadline = time.time() + timeout
    v = fn()
    while not v and time.time() < deadline:
        time.sleep(step)
        v = fn()
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "taskbar_peek_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="taskbar_peek_")
    res = Result()

    def shot(name):
        p = os.path.join(tmp, name + ".png")
        qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        for k in ("taskbar_buttons", "taskbar_align", "taskbar_float", "taskbar_peek", "start_position"):
            dbg.send(f"sh config unset desktop.{k}")
        for app in ("Task Manager", "Notepad", "Notepad"):
            dbg.open_app(app)
            time.sleep(1.5)
        wait_for(lambda: len(dbg.json("gui taskbar --json").get("buttons", [])) >= 3, 20)
        dbg.settle()
        tb = dbg.json("gui taskbar --json")
        focused = next((w["title"] for w in dbg.windows() if w.get("focused")), None)
        # A button that is NOT the focused window's, so activating it is
        # an observable change.
        target = next((b for b in tb["buttons"] if b["title"] == "Task Manager"), None)
        other = next((b for b in tb["buttons"] if b["title"] != "Task Manager"), None)
        if not target or not other:
            res.check("Task Manager and a second window are on the strip", False, str(tb["buttons"]))
            return report(res)

        def peek():
            return dbg.json("gui peek --json")

        # --- it opens, and shows the window ---------------------------
        print("preview (the default)")
        dbg.warp_cursor(qmp, 640, 300)
        time.sleep(0.5)
        dbg.warp_cursor(qmp, target["cx"], target["cy"])
        pk = wait_for(lambda: peek().get("open") and peek(), 4)
        res.check("resting on a button opens its preview",
                  bool(pk) and pk["mode"] == "preview" and pk["entries"]
                  and pk["entries"][0]["title"] == "Task Manager", f"peek: {pk}")
        if not pk or not pk.get("entries"):
            return report(res)
        e = pk["entries"][0]
        t = e["thumb"]
        res.check("the card sits above the strip, over its button",
                  pk["y"] + pk["h"] <= tb["y"] and pk["x"] < target["cx"] < pk["x"] + pk["w"],
                  f"card {pk['x']},{pk['y']} {pk['w']}x{pk['h']}, strip y {tb['y']}")
        time.sleep(0.5)
        im = shot("preview")
        px = [im.getpixel((t["x"] + t["w"] * i // 8, t["y"] + t["h"] * j // 8))
              for i in range(1, 8) for j in range(1, 8)]
        mean = sum(lum(p) for p in px) / len(px)
        spread = max(lum(p) for p in px) - min(lum(p) for p in px)
        res.check("the thumbnail is a picture: not black, not one flat colour",
                  t["w"] > 0 and mean > 60 and spread > 30,
                  f"thumb {t}, mean luminance {mean:.0f}, spread {spread}")

        # --- it switches at once, and closes after a grace -----------
        dbg.warp_cursor(qmp, other["cx"], other["cy"])
        time.sleep(0.3)
        pk2 = peek()
        res.check("moving to the next button switches the card at once",
                  pk2.get("open") and pk2["entries"] and pk2["entries"][0]["title"] == other["title"],
                  f"peek: {pk2}")
        dbg.warp_cursor(qmp, 640, 250)
        closed = wait_for(lambda: not peek().get("open"), 2)
        res.check("leaving for the desktop closes it", bool(closed), f"peek: {peek()}")

        # --- a click on the entry activates the window ----------------
        dbg.warp_cursor(qmp, target["cx"], target["cy"])
        pk = wait_for(lambda: peek().get("open") and peek(), 4)
        if pk and pk.get("entries"):
            t = pk["entries"][0]["thumb"]
            dbg.warp_cursor(qmp, t["x"] + t["w"] // 2, t["y"] + t["h"] // 2)
            time.sleep(0.3)
            dbg.click(t["x"] + t["w"] // 2, t["y"] + t["h"] // 2)
            time.sleep(0.8)
            now = next((w["title"] for w in dbg.windows() if w.get("focused")), None)
            res.check("clicking the preview activates that window, and the card closes",
                      now == "Task Manager" and not peek().get("open"),
                      f"focused {focused} -> {now}, peek open {peek().get('open')}")
        else:
            res.check("the preview reopens", False, str(pk))

        # --- highlight -------------------------------------------------
        print("highlight")
        dbg.send("sh config set desktop.taskbar_peek highlight")
        wait_for(lambda: peek().get("mode") == "highlight", 6)
        dbg.warp_cursor(qmp, 640, 250)
        time.sleep(0.8)
        # Wallpaper right of every window and above the build stamp; the
        # BRIGHT part of it, where a dim is a large difference.
        wall = (dbg.state()["screen"]["w"] - 8, tb["y"] - 90)
        before = shot("before-highlight")
        # TASK MANAGER, which the click above raised: a window already on
        # top keeps its pixels when lifted, so a changed pixel inside it
        # can only mean the lift drew it wrong. (Lifting a window that was
        # UNDER another rightly changes what is shown there.)
        dbg.warp_cursor(qmp, target["cx"], target["cy"])
        pk = wait_for(lambda: peek().get("open") and peek(), 4)
        if pk and pk.get("entries"):
            t = pk["entries"][0]["thumb"]
            dbg.warp_cursor(qmp, t["x"] + t["w"] // 2, t["y"] + t["h"] // 2)
            time.sleep(0.8)
            pk = peek()
            after = shot("highlight")
            lifted = next((w for w in dbg.windows() if w["title"] == target["title"]), None)
            inside = (lifted["x"] + lifted["w"] // 2, lifted["y"] + lifted["h"] // 2) if lifted else wall
            res.check("the WM reports the hovered entry's window as lifted",
                      pk.get("highlight") == target["title"], f"peek: {pk}")
            res.check("the desktop dims while the lifted window keeps its pixels",
                      lum(before.getpixel(wall)) > 30
                      and lum(after.getpixel(wall)) * 4 < lum(before.getpixel(wall)) * 3
                      and after.getpixel(inside) == before.getpixel(inside),
                      f"wallpaper {before.getpixel(wall)} -> {after.getpixel(wall)}, "
                      f"window {before.getpixel(inside)} -> {after.getpixel(inside)}")

            # ...and its x closes it.
            c = pk["entries"][0]["close"]
            n_before = len(dbg.windows())
            dbg.warp_cursor(qmp, c["x"] + c["s"] // 2, c["y"] + c["s"] // 2)
            time.sleep(0.3)
            dbg.click(c["x"] + c["s"] // 2, c["y"] + c["s"] // 2)
            gone = wait_for(lambda: len(dbg.windows()) < n_before, 4)
            res.check("the preview's x closes that window", bool(gone),
                      f"{n_before} windows before, {len(dbg.windows())} after")
        else:
            res.check("the preview opens in highlight mode", False, str(pk))

        # --- off -------------------------------------------------------
        print("off")
        dbg.send("sh config set desktop.taskbar_peek off")
        wait_for(lambda: peek().get("mode") == "off", 6)
        dbg.warp_cursor(qmp, 640, 250)
        time.sleep(0.4)
        dbg.warp_cursor(qmp, target["cx"], target["cy"])
        time.sleep(1.5)
        res.check("off opens no preview", not peek().get("open"), f"peek: {peek()}")

        dbg.warp_cursor(qmp, 640, 250)
        dbg.send("sh config unset desktop.taskbar_peek")
    return report(res)


def report(res):
    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\ntaskbar_peek_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
