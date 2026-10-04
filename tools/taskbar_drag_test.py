#!/usr/bin/env python3
"""tools/taskbar_drag_test.py -- dragging taskbar buttons to reorder
them, the glide, click-on-release, and a cross-window drag resting on a
button raising its window (userland/wm/wm_taskbar.h).

WHAT THIS ASSERTS THAT "THE ORDER CHANGED" WOULD NOT
---------------------------------------------------
* MID-DRAG, the layout the WM reports (`gui taskbar --json`, from the
  same taskbar_layout() that draws and hit-tests) has the dragged button
  under the pointer and the one it passed already in its old slot --
  and the dragged button is DRAWN lifted there, a pixel off the strip's
  colour where only the strip would otherwise be.
* THE DROP ACTIVATES NOTHING: a drag is not a click, so focus stays put.
* THE GLIDE IS TIME, NOT A JUMP: the drop adds frames to the WM's
  `glide_frames` counter and half a second later nothing is gliding;
  with `desktop.animations` off the counter does not move, and the
  order still commits. A COUNTER, because a 150 ms glide fits between
  two samples of a `gliding` flag -- the first version sampled one.
* A CLICK STILL WORKS, on release -- the press only arms the button now.
* A new window lands at the END of a reordered strip.
* A file dragged off the desktop and rested on a button brings that
  window forward, and is let go over the empty strip, where nothing
  takes a drop.

    python3 tools/vm.py --disk <copy> start
    python3 tools/taskbar_drag_test.py
    echo $?

POSITIVE CONTROL, both run: with apply_drag() not called, the three
mid-drag checks, the committed order and both glide checks go red
(nothing moves, so nothing glides); with the glide's retarget replaced
by a jump, "the drop glides" goes red and nothing else. Leaves animations unset
and removes its desktop file.
"""

import argparse
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results  # noqa: E402

DESK_FILE = "/home/desktop/zz-dragtest.txt"


Result = Results


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "taskbar_drag_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="taskbar_drag_")
    res = Result()

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        for k in ("taskbar_buttons", "taskbar_align", "start_position", "taskbar_float",
                  "taskbar_peek", "animations", "taskbar_combine"):
            dbg.send(f"sh config unset desktop.{k}")
        # Peek off: its card would open over the strip during the slow moves.
        dbg.send("sh config set desktop.taskbar_peek off")
        for app in ("File Manager", "Task Manager", "Notepad"):
            dbg.open_app(app)
            time.sleep(1.5)
        deadline = time.time() + 20
        while time.time() < deadline and len(dbg.json("gui taskbar --json").get("buttons", [])) < 3:
            time.sleep(0.5)
        dbg.settle()

        def tb():
            return dbg.json("gui taskbar --json")

        def titles():
            return [b["title"] for b in tb()["buttons"]]

        def focused():
            return next((w["title"] for w in dbg.windows() if w.get("focused")), None)

        def drag(src_title, past_title, inspect=None):
            """Press on one button, move right past another, release."""
            t = tb()
            src = next(b for b in t["buttons"] if b["title"] == src_title)
            past = next(b for b in t["buttons"] if b["title"] == past_title)
            dbg.warp_cursor(qmp, src["cx"], src["cy"])
            time.sleep(0.2)
            qmp.mouse_down()
            time.sleep(0.2)
            end = past["cx"] + past["w"] // 4
            for x in range(src["cx"], end + 1, 16):
                dbg.warp_cursor(qmp, x, src["cy"])
                time.sleep(0.03)
            dbg.warp_cursor(qmp, end, src["cy"])
            time.sleep(0.4)
            mid = inspect(src, end) if inspect else None
            qmp.mouse_up()
            return mid

        # --- a drag reorders, live -----------------------------------
        print("drag to reorder")
        before = titles()
        focus_before = focused()
        first, second = before[0], before[1]
        glides0 = tb()["glide_frames"]

        def inspect(src, px):
            t = tb()
            d = next((b for b in t["buttons"] if b.get("dragging")), None)
            im_path = os.path.join(tmp, "mid.png")
            qmp.screenshot(im_path)
            im = Image.open(im_path).convert("RGB")
            probe = im.getpixel((d["x"] + 3, t["btn_y"] + t["btn_h"] // 2)) if d else None
            return t, d, probe

        t, d, probe = drag(first, second, inspect)
        res.check("mid-drag the dragged button follows the pointer",
                  t["dragging"] and d is not None and d["title"] == first
                  and abs(d["x"] + (d["w"] // 2) - (t["buttons"][1]["cx"])) < d["w"],
                  f"dragging={t['dragging']} dragged={d}")
        res.check("...and the button it passed has slid into its slot",
                  t["buttons"][0]["title"] == second, f"row {[b['title'] for b in t['buttons']]}")
        res.check("...and it is drawn lifted, not as strip",
                  probe is not None and probe != rgb(t["bar"]), f"pixel {probe}, bar {rgb(t['bar'])}")
        time.sleep(0.6)
        after = tb()
        res.check("the drop glides: frames were drawn mid-glide",
                  after["glide_frames"] > glides0, f"glide_frames {glides0} -> {after['glide_frames']}")
        res.check("...and it has settled half a second later",
                  after.get("gliding") is False and not after.get("armed"), f"{after.get('gliding')}")
        res.check("the drop commits the order",
                  titles() == [second, first] + before[2:], f"{before} -> {titles()}")
        res.check("a drag activates nothing -- focus stays put",
                  focused() == focus_before, f"{focus_before} -> {focused()}")

        # --- with animations off, it jumps ----------------------------
        dbg.send("sh config set desktop.animations off")
        time.sleep(2.0)
        dbg.settle()
        before = titles()
        g0 = tb()["glide_frames"]
        drag(before[0], before[1])
        time.sleep(0.6)
        after = tb()
        res.check("animations off: the order commits with no glide",
                  after["glide_frames"] == g0 and titles()[:2] == [before[1], before[0]],
                  f"glide_frames {g0} -> {after['glide_frames']}, {before} -> {titles()}")
        dbg.send("sh config unset desktop.animations")
        time.sleep(1.0)

        # --- a click still works, on release --------------------------
        target = next(b for b in tb()["buttons"] if b["title"] != focused())
        dbg.click(target["cx"], target["cy"])
        time.sleep(0.8)
        res.check("a plain click still activates its window",
                  focused() == target["title"], f"clicked {target['title']}, focused {focused()}")

        # --- a new window joins at the end ----------------------------
        order = titles()
        dbg.open_app("Calculator")
        deadline = time.time() + 15
        while time.time() < deadline and len(titles()) <= len(order):
            time.sleep(0.5)
        res.check("a new window lands at the end of a reordered strip",
                  titles()[:len(order)] == order and titles()[-1] == "Calculator",
                  f"{order} -> {titles()}")

        # --- a cross-window drag resting on a button raises it --------
        print("drag-over raise")
        dbg.send(f"sh write {DESK_FILE} hello")
        time.sleep(2.0)
        dbg.settle()
        r = dbg.send("gui icons --json") or ""
        icons = json.loads(r[r.rfind('{"cached"'):]).get("icons", [])
        ic = next((i for i in icons if i["name"] == os.path.basename(DESK_FILE)), None)
        tgt = next(b for b in tb()["buttons"] if b["title"] != focused())
        if ic:
            cx, cy = ic["x"] + ic["w"] // 2, ic["y"] + ic["h"] // 3
            dbg.warp_cursor(qmp, cx, cy)
            time.sleep(0.2)
            qmp.mouse_down()
            time.sleep(0.2)
            for k in range(1, 13):
                dbg.warp_cursor(qmp, cx + (tgt["cx"] - cx) * k // 12, cy + (tgt["cy"] - cy) * k // 12)
                time.sleep(0.04)
            time.sleep(1.0)
            raised = focused()
            # LET GO OVER THE EMPTY STRIP: nothing there takes a drop.
            t = tb()
            dbg.warp_cursor(qmp, t["tray_x"] - 30, tgt["cy"])
            time.sleep(0.2)
            qmp.mouse_up()
            time.sleep(0.5)
            res.check("a file dragged onto a button, resting there, raises that window",
                      raised == tgt["title"], f"rested on {tgt['title']}, focused {raised}")
        else:
            res.check("the desktop shows the test file", False, str([i["name"] for i in icons]))
        dbg.send(f"sh rm {DESK_FILE}")
        dbg.send("sh config unset desktop.taskbar_peek")

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\ntaskbar_drag_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
