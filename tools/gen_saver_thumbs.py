#!/usr/bin/env python3
"""Draw each screensaver's gallery picture: data/wm/savers/<name>.jpg.

System Settings shows the savers as a gallery with a monitor above it
(mockup G2, 2026-10-08), and a saver is a separate PROGRAM -- Settings
cannot draw one into its own window, nor host one (Windows' preview runs
the .scr into a child window, which toy-os has no equivalent of). So each
saver ships a picture beside its option descriptor, as macOS screen
savers ship a thumbnail in their bundle, and this is what makes them: it
runs every program in /bin/wm/savers on a running VM, waits until it has
something to show, and saves a 480x270 frame.

A NEW SAVER NEEDS THIS RUN, or its card has no picture (a dark tile,
which is what a missing file draws).

    python3 tools/vm.py start
    python3 tools/gen_saver_thumbs.py            # every saver
    python3 tools/gen_saver_thumbs.py cube       # just these
    python3 tools/vm.py stop

The desktop the cube captures is whatever the VM shows: start from a
fresh image (tools/fresh_disk.py) so the picture is the default desktop.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
import screensaver_test as savers  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "wm", "savers")
# How long each saver needs before its frame is representative: the cube
# folds the desktop up first, the rest are drawing within a second or two.
SETTLE_S = {"cube": 9.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("names", nargs="*", help="savers to draw (default: all)")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "gen_saver_thumbs")
    from PIL import Image, ImageFilter, ImageStat

    qmp = QMPSession(port=args.qmp_port)
    enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    listed = [ln.strip() for ln in (dbg.send(f"sh ls {savers.SAVER_DIR}") or "").splitlines()
              if ln.strip() and ":" not in ln and "/" not in ln]
    names = args.names or listed
    was = next((ln.strip() for ln in (dbg.send("sh config get desktop.screensaver") or "").splitlines()
                if ln.strip() and " " not in ln.strip() and ":" not in ln and "=" not in ln), None)
    savers.set_setting(dbg, "desktop.screensaver_idle", "5")
    st = dbg.state()
    qmp.goto(st["screen"]["w"] - 2, 2)   # the pointer is in a screendump, not a capture
    tmp = os.path.join(ROOT, "build", "saver_thumb.png")
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    for name in names:
        savers.set_setting(dbg, "desktop.screensaver", name)
        dbg.send("gui idle start")
        if savers.wait_pid(dbg, True) is None:
            print(f"  {name}: did not start -- skipped")
            continue
        time.sleep(SETTLE_S.get(name, 4.0))
        qmp.screenshot(tmp, stable=False)
        dbg.key(ord("a"))
        savers.wait_pid(dbg, False)
        out = os.path.join(OUT, name + ".jpg")
        im = Image.open(tmp).convert("RGB")
        # A DARK FRAME OF SMALL POINTS (stars) shrinks to black: a pixel-
        # wide star is a quarter of a pixel at 480 wide. Grown first, so
        # each still reads; a saver that fills the screen is left alone.
        if sum(ImageStat.Stat(im).mean) / 3 < 16:
            im = im.filter(ImageFilter.MaxFilter(5))
        im.resize((480, 270), Image.LANCZOS).save(out, quality=85)
        print(f"  {name}: {os.path.relpath(out, ROOT)} ({os.path.getsize(out)} bytes)")
        time.sleep(1)
    savers.set_setting(dbg, "desktop.screensaver", was or "starfield")
    return 0


if __name__ == "__main__":
    sys.exit(main())
