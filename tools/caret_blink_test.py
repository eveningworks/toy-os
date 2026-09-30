#!/usr/bin/env python3
"""The text caret blinks, stops solid, and costs nothing once it stops.

ui/uui_caret.h: one blink phase per process, on for 500 ms and off for
500 ms after an input, SOLID ten seconds after the last one (GTK's
gtk-cursor-blink-timeout), and solid always with `desktop.caret_blink`
off. Driven in Notepad, whose editor draws its caret through utext.

What each check is for:
  - THE BLINK: several captures of the editor across two seconds must
    differ, and ONLY in a caret-sized band -- a whole-window difference
    would be a repaint of something else, not a blink.
  - THE STOP: ten seconds after the last key the captures must be
    identical AND show the caret (the ON frame), so an idle window sits
    still without losing its caret.
  - THE SETTING: off means identical captures with the caret showing.
  - THE COST: a window whose field LOST FOCUS mid-blink must not spin.
    That shipped once: the flip asked for a repaint, the repaint drew no
    caret, and the loop asked again forever -- 25% of a CPU per window.
    Measured as the app's CPU time across a few seconds, from `ps`.

gui_debug.enter_gui() turns blinking OFF for every other tool (a blink
defeats a settled-frame comparison), so this one does not use it and
sets the setting itself.
"""
import argparse
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, wait_for_desktop  # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

INTERVAL_S = 0.27   # does not divide the 500 ms half-period
SAMPLES = 8


Result = Results


def captures(qmp, tmp, box, tag, n=SAMPLES):
    from PIL import Image
    frames = []
    for i in range(n):
        path = qmp.screenshot(os.path.join(tmp, f"caret_{tag}{i}.png"),
                              stable=False)  # measuring motion
        frames.append(Image.open(path).convert("RGB").crop(box))
        time.sleep(INTERVAL_S)
    return frames


def digest(im):
    return hashlib.md5(im.tobytes()).hexdigest()[:10]


def ink(im):
    """Dark pixels: the caret is text-coloured on a white editor."""
    return sum(1 for p in im.getdata() if sum(p) < 200)


def cpu_of(dbg, pid):
    out = dbg.send("sh ps") or ""
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 6 and f[0] == str(pid):
            try:
                return float(f[4])
            except ValueError:
                return None
    return None


def run(dbg, qmp, tmp, res):
    from PIL import ImageChops
    dbg.send("sh config set desktop.caret_blink on")
    dbg.open_app("Notepad")
    dbg.settle()
    time.sleep(0.5)
    w = dbg.window("untitled")
    if not res.check("Notepad opened", bool(w)):
        return
    c = w["content"]
    # The editor's text area: below the menu bar, left of the scrollbar.
    box = (c["x"] + 2, c["y"] + 24, c["x"] + c["w"] - 24, c["y"] + c["h"] // 2)
    qmp.click_at(c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
    qmp.send_text("hello")
    time.sleep(0.3)

    # --- the blink ---------------------------------------------------
    frames = captures(qmp, tmp, box, "blink")
    kinds = sorted({digest(f) for f in frames})
    if not res.check("the caret blinks: the editor changes across 2 s",
                     len(kinds) >= 2, f"{len(kinds)} distinct in {SAMPLES}"):
        return
    a = frames[0]
    b = next(f for f in frames if digest(f) != digest(a))
    bb = ImageChops.difference(a, b).getbbox()
    bw, bh = (bb[2] - bb[0], bb[3] - bb[1]) if bb else (0, 0)
    res.check("...and only in a caret-sized band", bb and bw <= 4 and bh <= 40,
              f"diff {bw}x{bh} at {bb}")
    on = a if ink(a) > ink(b) else b

    # --- the stop ----------------------------------------------------
    time.sleep(10.5)
    idle = captures(qmp, tmp, box, "idle", n=6)
    res.check("ten seconds after the last key the editor sits still",
              len({digest(f) for f in idle}) == 1,
              f"{len({digest(f) for f in idle})} distinct in 6")
    res.check("...with the caret SHOWING, not hidden",
              digest(idle[0]) == digest(on))

    # --- the setting -------------------------------------------------
    dbg.send("sh config set desktop.caret_blink off")
    qmp.send_key("end")   # an input re-reads the setting; End moves nothing here
    time.sleep(0.3)
    off = captures(qmp, tmp, box, "off")
    res.check("desktop.caret_blink off: the editor sits still while typing",
              len({digest(f) for f in off}) == 1,
              f"{len({digest(f) for f in off})} distinct in {SAMPLES}")
    res.check("...with the caret showing", digest(off[0]) == digest(on))

    # --- the cost of a field that lost focus mid-blink ----------------
    dbg.send("sh config set desktop.caret_blink on")
    qmp.send_text("x")
    time.sleep(0.6)                 # into the blink
    dbg.open_app("Calculator")      # takes the focus from Notepad
    dbg.settle()
    pid = w.get("client_pid")
    t0 = cpu_of(dbg, pid)
    time.sleep(4.0)
    t1 = cpu_of(dbg, pid)
    used = (t1 - t0) if (t0 is not None and t1 is not None) else None
    res.check("a window that lost focus mid-blink does not spin",
              used is not None and used < 0.3,
              f"pid {pid}: {used} s of CPU in 4 s")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--in-gui", action="store_true")   # accepted; nothing to enter
    args = ap.parse_args()
    port_guard.resolve_instance(args, "caret_blink_test")

    wait_for_desktop(args.sock)
    qmp = QMPSession(port=args.qmp_port)
    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, res)
    finally:
        try:
            dbg.send("sh config set desktop.caret_blink on")
        except Exception:
            pass
        dbg.close()

    print(f"\ncaret_blink_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
