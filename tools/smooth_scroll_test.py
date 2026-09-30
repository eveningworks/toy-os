#!/usr/bin/env python3
"""Smooth scrolling (ui/uui_scrollanim.h) in the File Manager's icon
grid, and the `desktop.smooth_scroll` setting that turns it off.

One wheel notch with the setting ON must produce SEVERAL drawn frames at
distinct positions before the grid settles; with it OFF, exactly one.
The positions come from the app's own `files: layout cellgrid` report,
which it emits once per drawn frame that differs from the last -- read
back through `dmesg`, whose timestamps also say the frames were spread
over time rather than drawn in one burst. Both halves are asserted, so
a glide that ran but never reached the screen, and a setting that was
saved but never read, each fail their own check.

Also: a thumb drag mid-list moves the grid on the FIRST frame after the
motion (a drag never glides -- docs/gui-guidelines.md's scrollbar rule
2), and the setting is left ON afterwards, since it is written to
/etc/desktop.conf and would otherwise change the machine for every
later tool (CLAUDE.md).

    python3 tools/vm.py start
    python3 tools/smooth_scroll_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                  # noqa: E402
import port_guard                                # noqa: E402
from harness import Results  # noqa: E402

SPAWN = "/bin/wm/apps/files"
TITLE = "File Manager"
DIR = "/smoothtest"
FILES = 40          # enough grid rows to scroll at the default window size
GLIDE_S = 0.7       # comfortably past UUI_SCROLL_MS plus a slow frame or two

TS = re.compile(r"^\[(\d+\.\d+)\]")


Result = Results


def cell_frames(dbg, pane, since):
    """(timestamp, y) of every `layout cellgrid <pane>` line after `since`."""
    out = []
    for line in dbg.send("sh dmesg").splitlines():
        m = TS.match(line)
        if not m or f"files: layout cellgrid {pane} " not in line:
            continue
        ts = float(m.group(1))
        if ts <= since:
            continue
        parts = line.split(f"files: layout cellgrid {pane} ", 1)[1].split()
        try:
            out.append((ts, int(parts[1])))
        except (IndexError, ValueError):
            continue
    return out


def last_ts(dbg):
    ts = [float(m.group(1)) for m in (TS.match(l) for l in dbg.send("sh dmesg").splitlines()) if m]
    return ts[-1] if ts else 0.0


def distinct(seq):
    out = []
    for v in seq:
        if not out or out[-1] != v:
            out.append(v)
    return out


def icons_pane(dbg):
    """The pane index that reports a cell grid (the icons view), and its
    rect -- THE WHEEL GOES TO THE PANE UNDER THE POINTER, so the pane
    measured must be the pane pointed at. Measuring pane 1 while
    scrolling pane 0 reads a moving grid as still (it did: pane 1's
    lines re-emit with every frame of pane 0's glide, all at one y)."""
    lines = dbg.send("sh dmesg").splitlines()
    # THE ACTIVE PANE, not the last one to report a grid: a hidden second
    # pane reports the same rect as the visible one, and the wheel goes
    # to the active pane -- measuring the hidden twin read a working
    # glide as a still grid.
    pane = None
    for line in reversed(lines):
        if "files: layout active " in line:
            pane = int(line.split("files: layout active ", 1)[1].split()[0])
            break
    if pane is None or not any(f"files: layout cellgrid {pane} " in l for l in lines):
        return None, None
    for line in reversed(lines):
        key = f"files: layout pane {pane} "
        if key in line:
            rect = tuple(int(v) for v in line.split(key, 1)[1].split()[:4])
            return pane, rect
    return pane, None


def run(dbg, qmp, res):
    dbg.send("sh config set desktop.layout_log on")
    dbg.send(f"sh mkdir {DIR}")
    for i in range(FILES):
        dbg.send(f"sh touch {DIR}/s{i:02}.txt")
    win = dbg.spawn(f"{SPAWN} {DIR} {DIR}", TITLE)
    res.check("the File Manager opens over the fixture", win is not None)
    if not win:
        return
    c = win["content"]
    time.sleep(1.0)
    pane, pane_rect = icons_pane(dbg)
    res.check("a pane is in icons view and reports its grid and rect",
              pane is not None and pane_rect is not None, f"pane={pane} rect={pane_rect}")
    if pane is None or pane_rect is None:
        return
    # Point INTO THE ACTIVE PANE: the wheel goes to it.
    px, py, pw, ph = pane_rect
    dbg.warp_cursor(qmp, c["x"] + px + pw // 2, c["y"] + py + ph // 2)
    time.sleep(0.3)
    start_y = cell_frames(dbg, pane, 0.0)[-1][1]

    try:
        for setting, want_many in (("on", True), ("off", False), ("on", True)):
            dbg.send(f"sh config set desktop.smooth_scroll {setting}")
            time.sleep(0.3)
            since = last_ts(dbg)
            qmp.wheel("down", 1)
            time.sleep(GLIDE_S)
            frames = cell_frames(dbg, pane, since)
            ys = distinct([y for _, y in frames])
            spread = (frames[-1][0] - frames[0][0]) if len(frames) > 1 else 0.0
            if want_many:
                res.check(f"smooth_scroll={setting}: one notch draws several frames at distinct positions",
                          len(ys) >= 3, f"positions={ys}")
                res.check(f"smooth_scroll={setting}: ...spread over time, not one burst",
                          spread >= 0.03, f"spread={spread:.3f}s frames={frames}")
                if len(ys) >= 3:
                    steps = [abs(ys[i] - ys[i - 1]) for i in range(1, len(ys))]
                    res.check(f"smooth_scroll={setting}: ...and easing OUT (steps shrink)",
                              steps[0] >= steps[-1], f"steps={steps}")
            else:
                res.check(f"smooth_scroll={setting}: one notch draws exactly one new frame, and it moved",
                          len(ys) == 1 and ys[0] != start_y, f"positions={ys} start={start_y}")
            # Back up, so each round starts from the same place.
            qmp.wheel("up", 1)
            time.sleep(GLIDE_S)

        # A drag does not glide: grab the thumb and move it; the first
        # frame after the move is already at the dragged position, and
        # no frames follow it.
        from PIL import Image
        shot = os.path.join(os.environ.get("TMPDIR", "/tmp"), "smooth_scroll_bar.png")
        qmp.screenshot(shot)
        im = Image.open(shot).convert("RGB")
        # Re-read the rect: a reload could have re-laid the panes.
        _, pane_rect = icons_pane(dbg)
        res.check("the icons pane still reports its rect", pane_rect is not None)
        if pane_rect:
            px, py, pw, ph = pane_rect
            bx = c["x"] + px + pw - 7
            thumb = [y for y in range(c["y"] + py, c["y"] + py + ph)
                     if im.getpixel((bx, y)) == (150, 155, 165)]
            res.check("the thumb is on screen in its known colour", len(thumb) > 8, f"rows={len(thumb)}")
            if len(thumb) > 8:
                ty = thumb[0] + 6
                dbg.warp_cursor(qmp, bx, ty)
                qmp.mouse_down()
                time.sleep(0.2)
                since = last_ts(dbg)
                dbg.warp_cursor(qmp, bx, ty + 24)
                time.sleep(GLIDE_S)
                qmp.mouse_up()
                frames = cell_frames(dbg, pane, since)
                ys = distinct([y for _, y in frames])
                res.check("a thumb drag lands in one frame -- it never glides",
                          1 <= len(ys) <= 2, f"positions={ys}")
    finally:
        # THE SETTING IS ON DISK. Leave it as shipped, or every later tool
        # on this image scrolls without the glide it is meant to see.
        dbg.send("sh config set desktop.smooth_scroll on")
        for p in dbg.processes():
            if p["name"] == "files" and p["state"] != "zombie":
                dbg.send(f"sh kill {p['pid']}")
        dbg.send(f"sh rm -r {DIR}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "smooth_scroll_test")
    if args.logs:
        os.makedirs(args.logs, exist_ok=True)
        os.environ["TMPDIR"] = args.logs

    res = Result()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nsmooth_scroll_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
