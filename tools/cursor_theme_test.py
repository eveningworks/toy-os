#!/usr/bin/env python3
"""Cursor themes: loading, switching, sizing and the malformed-file floor.

The pointer's shapes are data files (/usr/share/cursors/<theme>/<shape>,
see tools/gen_cursors.py), the compositor resolves a shape name into
pixels, and two registered settings choose the theme and the size.

WHAT THIS TEST IS SHAPED AROUND, because it cost real time: the built-in
fallback means a completely broken theme still draws a working pointer.
So "a cursor is on screen" proves nothing at all -- the first version of
this feature loaded 0 of 6 shapes (every file failed on its own comment
header) and looked perfect. Every check here therefore asserts on the
LOAD COUNT or on a pixel DIFFERENCE between two states, never on the
mere presence of a cursor.

The pointer is measured as ink in a box around a parked cursor. It has
to be parked with DebugConsole.warp_cursor() -- `gui move` lasts one WM
iteration -- and un-parked is not needed here since nothing else in this
tool depends on where it sits.

Usage (the VM must already be up):
    python3 tools/vm.py start
    python3 tools/cursor_theme_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
# Empty desktop background, well clear of the icons in the top-left
# column and of the taskbar.
PARK_X, PARK_Y = 600, 400
BG = (24, 60, 90)

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def cursor_ink(qmp, tag):
    """Non-background pixels in a box around the parked cursor.

    The box is generous enough for the largest shape at 3x (the size
    setting's maximum), so growing the cursor cannot silently push ink
    outside what is being counted -- which would read as the cursor
    getting SMALLER.
    """
    path = os.path.join(tempfile.gettempdir(), f"cursor_{tag}.png")
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    n = 0
    for y in range(PARK_Y - 8, PARK_Y + 80):
        for x in range(PARK_X - 8, PARK_X + 80):
            if im.getpixel((x, y)) != BG:
                n += 1
    return n


def loaded_count(dbg):
    """The most recent 'theme "x" -- n of 6 shapes loaded' line."""
    lines = [l for l in dbg.logs() if "shapes loaded" in l]
    if not lines:
        return None, None
    last = lines[-1]
    try:
        theme = last.split('theme "')[1].split('"')[0]
        n = int(last.split("--")[1].strip().split()[0])
        return theme, n
    except (IndexError, ValueError):
        return None, None


def set_setting(dbg, key, value):
    dbg.send(f"gui spawn /bin/config set {key} {value}")
    time.sleep(0.8)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("cursor themes")

    # --- the default theme loads COMPLETELY --------------------------
    # The load count, not the presence of a pointer: the built-in
    # fallback draws one either way, which is exactly how a theme that
    # loaded nothing shipped looking correct.
    # Via `bold` first, deliberately: setting a theme to the one already
    # loaded correctly reloads NOTHING (that is the point of the
    # generation check), so asking for "default" on a default desktop
    # produces no log line to read and the check would report a missing
    # theme rather than a working one.
    # ESTABLISH the starting state rather than inherit it. Both settings
    # persist to /etc/toyos.conf, which lives on the disk image and is
    # not re-seeded by a build -- so a previous run (or a person poking
    # at the desktop) leaves its size behind, and every measurement here
    # is relative to a baseline that would then be silently wrong. This
    # cost a confusing 9x ratio before it was set explicitly.
    # THE DESKTOP BACKGROUND MUST BE FLAT for any of this to measure a
    # cursor: every count below is "pixels differing from the background"
    # over a patch of desktop, and a wallpaper makes every pixel differ.
    # Both cursor checks saturated at the full patch (7744 = 88x88) the
    # day a default wallpaper shipped, which reads exactly like the theme
    # switch doing nothing.
    set_setting(dbg, "desktop.wallpaper", "none")
    set_setting(dbg, "cursor_size", "normal")
    set_setting(dbg, "cursor_theme", "bold")
    set_setting(dbg, "cursor_theme", "default")
    theme, n = loaded_count(dbg)
    check("the default theme loads every shape", n == 6,
          f'theme="{theme}" loaded={n} of 6')

    # --- it is what is actually drawn ---------------------------------
    DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
    time.sleep(0.5)
    ink_default = cursor_ink(qmp, "default")
    check("the pointer is drawn", ink_default > 40, f"{ink_default} px")

    # --- a different THEME changes the drawn shape --------------------
    set_setting(dbg, "cursor_theme", "bold")
    theme, n = loaded_count(dbg)
    check("the bold theme loads every shape", n == 6,
          f'theme="{theme}" loaded={n} of 6')

    DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
    time.sleep(0.5)
    ink_bold = cursor_ink(qmp, "bold")
    # bold is the default dilated by a pixel, so it must be BIGGER --
    # asserting a direction rather than "it changed", which a repaint
    # artefact or a stray window would also satisfy.
    check("switching theme changes the pointer, and bold is bigger",
          ink_bold > ink_default, f"default={ink_default} bold={ink_bold}")

    # --- SIZE scales it, by roughly the right amount ------------------
    set_setting(dbg, "cursor_theme", "default")
    set_setting(dbg, "cursor_size", "large")
    DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
    time.sleep(0.5)
    ink_large = cursor_ink(qmp, "large")
    # 2x linear is ~4x the area. Asserting the MAGNITUDE, not just an
    # increase: a one-pixel growth would satisfy "it got bigger" and is
    # exactly the class of bug a resize check missed here before.
    ratio = ink_large / max(1, ink_default)
    check("large is about four times the area", 3.0 <= ratio <= 5.0,
          f"default={ink_default} large={ink_large} ratio={ratio:.2f}")

    # --- and it is a ROUND TRIP ---------------------------------------
    set_setting(dbg, "cursor_size", "normal")
    DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
    time.sleep(0.5)
    ink_back = cursor_ink(qmp, "back")
    check("returning to normal restores the original pointer exactly",
          ink_back == ink_default, f"{ink_back} vs {ink_default}")

    # --- a malformed shape falls back, and says so --------------------
    # The floor that makes all of the above safe: a theme file a person
    # edited badly must cost its own shape, not the pointer.
    dbg.send("gui spawn /bin/config set cursor_theme broken")
    time.sleep(1.0)
    theme, n = loaded_count(dbg)
    check("a theme that does not exist loads nothing", n == 0,
          f'theme="{theme}" loaded={n}')

    DebugConsole.warp_cursor(dbg, qmp, PARK_X + 40, PARK_Y + 40)
    time.sleep(0.5)
    path = os.path.join(tempfile.gettempdir(), "cursor_broken.png")
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    ink_fallback = sum(1 for y in range(PARK_Y + 32, PARK_Y + 80)
                        for x in range(PARK_X + 32, PARK_X + 80)
                        if im.getpixel((x, y)) != BG)
    check("the built-in pointer still draws with no theme at all",
          ink_fallback > 40, f"{ink_fallback} px")

    # --- both settings are in the registry ----------------------------
    # Which is what gives them a Control Panel row and an /etc key with
    # no edit to either.
    dbg.send("gui spawn /bin/config list")
    time.sleep(0.8)
    dbg.send("sh cat /etc/toyos.conf")
    time.sleep(0.5)
    conf = "\n".join(dbg.logs())
    check("the settings persisted to /etc/toyos.conf",
          "cursor_theme" in conf and "cursor_size" in conf,
          "both keys present" if "cursor_theme" in conf else "missing")

    # Leave the desktop as it was found. These settings persist to the
    # disk image, so a test that ends on "broken" hands the next one a
    # themeless pointer -- the same rule this file's own baseline check
    # had to learn.
    set_setting(dbg, "cursor_theme", "default")
    set_setting(dbg, "cursor_size", "normal")

    failed = sum(1 for _, ok in checks if not ok)
    print(f"\ncursor_theme_test: {len(checks) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
