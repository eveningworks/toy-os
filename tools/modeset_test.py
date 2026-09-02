#!/usr/bin/env python3
"""A runtime resolution change under a live desktop.

WHAT IS UNDER TEST
------------------
`config set resolution WxH` (kernel/lib/display_config.c) runs
screen_set_mode() (kernel/core/screen.c): the display driver sets the
mode, gfx and the console re-plumb, the compositor's framebuffer grant
is re-mapped, and WIN_EV_SCREEN makes the desktop re-lay out. Every
QEMU adapter can do it (bochs is the default `-vga std`), so this runs
in the ordinary suite.

WHAT IT ASSERTS, AND THE ORACLE
-------------------------------
1. THE DEVICE AGREES. A QMP screendump's own pixel size is QEMU's
   scanout geometry -- measured at the device, not read back from the
   guest -- and it must equal both the requested mode and what
   `gui state` believes. The two disagreeing is exactly the stale
   gfx_width() bug this work replaces.
2. THE DESKTOP STILL DRAWS AND STILL RESPONDS at the new size: the
   Start menu opens and paints (pixels change), a window can be
   maximized and its frame fills the NEW screen, and the far corner --
   outside the old mode entirely -- carries the window's pixels.
3. IT COMES BACK: the original mode is restored the same way, and the
   setting reads back through the hardware (`config get`).
4. A MODE THE DRIVER DOES NOT LIST IS REFUSED, and the screen is
   untouched by the refusal.

The setting is restored at the end, and its /etc value with it: a
stored resolution is applied at the next boot, so a tool that left one
behind would change the machine for every later boot.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/modeset_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TARGET = (1600, 900)

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def shot_size(qmp, path):
    from PIL import Image
    qmp.stable_pixels(path)
    return Image.open(path).size


def screen(dbg):
    s = dbg.state()["screen"]
    return (s["w"], s["h"])


def set_res(dbg, w, h):
    reply = (dbg.send(f"sh config set resolution {w}x{h}") or "")
    time.sleep(1.0)
    dbg.settle()
    return reply


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.abspath(os.path.join(outdir, n))  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("runtime resolution change (config set resolution -> screen_set_mode)")

    orig = screen(dbg)
    dev0 = shot_size(qmp, shot("mode_before.png"))
    check("the device and the desktop agree on the boot mode", dev0 == orig,
          f"device {dev0} desktop {orig}")
    # A boot already at the target (a stored resolution from an earlier,
    # interrupted run) tests the other direction instead of skipping.
    target = TARGET if orig != TARGET else (1280, 720)
    try:
        run(dbg, qmp, shot, orig, target)
    finally:
        # RESTORED WHATEVER HAPPENED: the value persists to /etc and is
        # applied at the next boot, so a tool that died mid-way would
        # change the machine for every later tool and every later boot.
        if screen(dbg) != orig:
            set_res(dbg, *orig)
    return report()


def run(dbg, qmp, shot, orig, TARGET):

    # --- 1. the change, measured at the device -------------------------
    reply = set_res(dbg, *TARGET)
    dev1 = shot_size(qmp, shot("mode_after.png"))
    now = screen(dbg)
    check(f"the device shows {TARGET[0]}x{TARGET[1]}", dev1 == TARGET, f"device {dev1}")
    check("...and the desktop believes the same", now == TARGET, f"desktop {now}")
    stored = (dbg.send("sh config get resolution") or "").strip().splitlines()
    stored = next((ln.strip() for ln in stored if "x" in ln), "")
    check("...and the setting reads back through the hardware",
          stored == f"{TARGET[0]}x{TARGET[1]}", stored or reply.strip()[-80:])

    # --- 2. the desktop still draws and responds -----------------------
    tb = dbg.json("gui taskbar --json")["start"]
    before = qmp.stable_pixels(shot("mode_menu_closed.png"))
    dbg.send(f"gui click {tb['cx']} {tb['cy']}")
    dbg.settle(); time.sleep(0.4)
    opened = qmp.stable_pixels(shot("mode_menu_open.png"))
    check("the Start menu opens at the new size",
          dbg.json("gui state --json")["overlays"]["start_menu"] is True)
    check("...and it is PAINTED", opened != before)
    dbg.send(f"gui click {tb['cx']} {tb['cy']}")
    dbg.settle(); time.sleep(0.3)

    dbg.open_app("Notepad")
    dbg.settle(); time.sleep(1.0)
    wins = dbg.windows()   # the newest window is last, as hires_test.py reads it
    if check("Notepad opened at the new size", bool(wins), str(wins)[:80]):
        win = wins[-1]
        # Maximize by LABEL from the title-bar context menu -- hires_test's
        # helper, so nothing here measures the chrome.
        dbg.rclick(win["x"] + win["w"] // 2, win["y"] + 6)
        dbg.settle()
        row = dbg.ctxmenu_row("Maximize")
        if check("the title-bar context menu offers Maximize", row is not None):
            dbg.click(*row)
            dbg.settle(); time.sleep(1.0)
            m = next((w for w in dbg.windows() if w["title"] == win["title"]), None)
            tbh = dbg.state()["taskbar_h"]
            check("a maximized frame fills the NEW screen",
                  m is not None and m["w"] == TARGET[0] and m["h"] == TARGET[1] - tbh,
                  str({k: m.get(k) for k in ("x", "y", "w", "h")}) if m else "no window")
            from PIL import Image
            qmp.stable_pixels(shot("mode_max.png"))
            im = Image.open(shot("mode_max.png")).convert("RGB")
            far = im.getpixel((TARGET[0] - 20, TARGET[1] - tbh - 20))
            bar = im.getpixel((TARGET[0] // 2, TARGET[1] - tbh // 2))
            check("the far corner, outside the old mode, carries the window, not the taskbar",
                  far != bar, f"corner {far} taskbar {bar}")
        dbg.send(f"gui close {len(dbg.windows()) - 1}")   # by index, newest last
        dbg.settle(); time.sleep(0.5)

    # --- 3. a refusal leaves the screen alone ---------------------------
    reply = (dbg.send("sh config set resolution 777x555") or "")
    time.sleep(0.5)
    check("an unlisted mode is refused", "exit 1" in reply or "refus" in reply.lower()
          or "invalid" in reply.lower(), reply.strip().replace("\r", " ")[-100:])
    check("...and the screen is untouched", screen(dbg) == TARGET and
          shot_size(qmp, shot("mode_refused.png")) == TARGET)

    # --- 4. and back ----------------------------------------------------
    set_res(dbg, *orig)
    dev2 = shot_size(qmp, shot("mode_back.png"))
    check("the boot mode is restored at the device", dev2 == orig, f"device {dev2}")
    check("...and in the desktop", screen(dbg) == orig, str(screen(dbg)))


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nmodeset_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
