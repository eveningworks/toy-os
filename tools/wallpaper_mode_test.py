#!/usr/bin/env python3
"""`desktop.wallpaper_mode`: does fit differ from fill, and is the
picture decoded only when it actually changes?

WHY THIS TOOL SETS A RESOLUTION
-------------------------------
It has to, or it measures nothing. Both stock wallpapers are exactly
1280x720 and so is the default mode, and at a matching aspect ratio
UIMG_FIT_CONTAIN and UIMG_FIT_COVER produce the SAME pixels -- so every
existing check of `wallpaper_mode` passes whether the setting works or
not. This runs at 1024x768 (4:3 against the picture's 16:9), where
`fit` letterboxes and `fill` crops, and check 1 asserts the premise
rather than assuming it.

WHAT IS UNDER TEST
------------------
`wallpaper_reload()` (userland/wm/desktop.c) re-reads two settings on a
filesystem generation bump and re-fits or reloads the picture.
`uui_image_draw()` derives the drawn size from `fit` every frame and
rescales its own cache, so ONE decoded source serves either mode.

THE PAIR THAT MAKES THIS MEAN SOMETHING. Check 4 asserts a mode change
logs no decode, and on its own that passes just as well if the log line
were broken or the wallpaper never loaded at all. Check 6 changes the
NAME and requires a decode to appear, from the same log, in the same
run. Neither half is evidence without the other.

The round trip (check 5) is the other assertion a weaker version would
miss: `fit` then `fill` must restore the ORIGINAL bytes, which "the
pixels changed" cannot distinguish from "the pixels changed to
something else".

EVERY SETTING IS RESTORED, resolution included -- a tool that leaves one
changed changes the machine for every later tool (CLAUDE.md), and this
one changes the screen size.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/wallpaper_mode_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402

# 4:3 against the wallpapers' 16:9, and in display.c's mode list.
TARGET = (1024, 768)
TASKBAR_BAND = 60   # rows at the bottom to keep out of the wallpaper box

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def setting(dbg, key):
    """One setting's value, read through the kernel shell.

    `gui spawn /bin/config` would repaint the screen, and every check
    here compares captures -- the same reason imgview_test.py uses the
    kernel shell for this.
    """
    out = dbg.send(f"sh config get {key}") or ""
    for line in out.splitlines():
        line = line.strip()
        if line and "=" not in line and ":" not in line and " " not in line:
            return line
    return ""


def set_setting(dbg, key, value):
    dbg.send(f"sh config set {key} {value}")
    time.sleep(0.8)


def screen_size(dbg):
    s = dbg.state()["screen"]
    return (s["w"], s["h"])


def set_res(dbg, w, h):
    dbg.send(f"sh config set resolution {w}x{h}")
    time.sleep(1.0)
    dbg.settle()


def decoded(dbg):
    """Did the WM decode a wallpaper since the last logs() call?

    desktop.c logs `desktop: wallpaper <name> WxH mode <m> in N ticks`
    only after uimg_load() returns, so the line's presence IS the decode.
    """
    return [l.strip() for l in dbg.logs() if "desktop: wallpaper" in l and " in " in l]


def wallpaper_box(w, h):
    """The wallpaper's own area. The taskbar is excluded because its
    clock ticks once a second and a box containing it can never settle."""
    return (0, 0, w, h - TASKBAR_BAND)


def taskbar_box(w, h):
    """A band of the taskbar that is chrome and nothing else.

    BOTH ENDS ARE EXCLUDED ON PURPOSE. The clock is at the right and
    ticks once a second, so a box containing it can never settle -- the
    same reason calendar_test.py keeps its panel box clear of it -- and
    the Start button at the left carries a hover and a flash state.
    What is left is the empty middle strip, which the wallpaper must
    not reach.
    """
    return (w // 4, h - TASKBAR_BAND // 2, w // 2, h - TASKBAR_BAND // 4)


def run(dbg, qmp, shot):
    # ESTABLISH the picture and the mode rather than inheriting whatever
    # the last tool left: wallpaper and wallpaper_mode both persist.
    set_setting(dbg, "desktop.wallpaper", "aurora")
    set_setting(dbg, "desktop.wallpaper_mode", "fill")

    # --- 1. the premise: at 16:9 the two modes are INDISTINGUISHABLE --
    # Asserted, not assumed. If a future wallpaper or default mode makes
    # these differ, this tool's reason for changing the resolution is
    # gone and the next reader should be told so here.
    w, h = screen_size(dbg)
    same_fill = qmp.stable_pixels(shot("wpm_16x9_fill.png"), box=wallpaper_box(w, h))
    set_setting(dbg, "desktop.wallpaper_mode", "fit")
    same_fit = qmp.stable_pixels(shot("wpm_16x9_fit.png"), box=wallpaper_box(w, h))
    check("at the boot mode fit and fill are pixel-identical (why this tool sets a mode)",
          same_fill == same_fit, f"{w}x{h}")
    set_setting(dbg, "desktop.wallpaper_mode", "fill")

    # --- the real measurement, at an aspect the picture does not match -
    set_res(dbg, *TARGET)
    now = screen_size(dbg)
    if not check(f"the screen is {TARGET[0]}x{TARGET[1]}", now == TARGET, f"{now}"):
        return
    w, h = now
    wbox, tbox = wallpaper_box(w, h), taskbar_box(w, h)

    set_setting(dbg, "desktop.wallpaper_mode", "fill")
    fill = qmp.stable_pixels(shot("wpm_fill.png"), box=wbox)
    fill_tb = qmp.stable_pixels(shot("wpm_fill_tb.png"), box=tbox)

    # --- 2/3. fit differs from fill, and only in the wallpaper --------
    dbg.logs()   # clear, so `decoded()` sees only this change
    set_setting(dbg, "desktop.wallpaper_mode", "fit")
    time.sleep(1.2)
    dec_mode = decoded(dbg)
    fit = qmp.stable_pixels(shot("wpm_fit.png"), box=wbox)
    fit_tb = qmp.stable_pixels(shot("wpm_fit_tb.png"), box=tbox)
    check("fit renders differently from fill", fit != fill)
    check("...and the taskbar is untouched (the control)", fit_tb == fill_tb)

    # --- 4. a MODE change does not decode -----------------------------
    check("a mode change decodes nothing -- the same source serves both fits",
          not dec_mode, "; ".join(dec_mode)[:110])

    # --- 5. and it is reversible, byte for byte -----------------------
    set_setting(dbg, "desktop.wallpaper_mode", "fill")
    back = qmp.stable_pixels(shot("wpm_back.png"), box=wbox)
    check("fit -> fill restores the original pixels exactly", back == fill)

    # --- 6. a NAME change DOES decode ---------------------------------
    # Without this, check 4 passes for a broken log line or an absent
    # wallpaper just as happily as for the behaviour it means to assert.
    dbg.logs()
    set_setting(dbg, "desktop.wallpaper", "dusk")
    time.sleep(1.2)
    dec_name = decoded(dbg)
    check("a NAME change DOES decode -- so check 4 is about the mode, not the log",
          bool(dec_name), "; ".join(dec_name)[:110] or "no decode logged")
    other = qmp.stable_pixels(shot("wpm_dusk.png"), box=wbox)
    check("...and the other wallpaper is a different picture", other != back)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "wallpaper_mode_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.abspath(os.path.join(outdir, n))  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("wallpaper_mode (fit vs fill, and when the picture is decoded)")

    orig_res = screen_size(dbg)
    orig_name = setting(dbg, "desktop.wallpaper")
    orig_mode = setting(dbg, "desktop.wallpaper_mode")
    try:
        run(dbg, qmp, shot)
    finally:
        # RESTORED WHATEVER HAPPENED -- all three persist to /etc, and
        # the resolution changes the screen every later tool sees.
        if orig_name:
            set_setting(dbg, "desktop.wallpaper", orig_name)
        if orig_mode:
            set_setting(dbg, "desktop.wallpaper_mode", orig_mode)
        if screen_size(dbg) != orig_res:
            set_res(dbg, *orig_res)
    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nwallpaper_mode_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
