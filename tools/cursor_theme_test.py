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
import re
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

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


def expected_shapes():
    """CURSOR_SHAPE_COUNT, read from the header that defines it.

    IT WAS THE LITERAL 6, and adding a shape reddened this tool with
    `loaded=7 of 6` -- a count restated in a second file goes stale the
    first time the first file moves (CLAUDE.md).
    """
    here = os.path.dirname(os.path.abspath(__file__))
    hdr = os.path.join(here, "..", "userland", "wm", "cursor_theme.h")
    with open(hdr) as f:
        m = re.search(r"#define\s+CURSOR_SHAPE_COUNT\s+(\d+)", f.read())
    return int(m.group(1)) if m else None


def loaded_count(dbg):
    """The most recent 'theme "x" -- n of N shapes loaded' line."""
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


CURSOR_DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "cursors")


def image_themes():
    """The themes whose arrow is an IMAGE shape, read from the data the
    image is built from -- so a new image theme is tested with no edit."""
    out = []
    for t in sorted(os.listdir(CURSOR_DATA)):
        p = os.path.join(CURSOR_DATA, t, "arrow")
        if os.path.exists(p) and "image=" in open(p).read():
            out.append(t)
    return out


def own_colours(theme):
    """The arrow's fully opaque colours, from its own 1x QOI.

    An opaque pixel composites to exactly itself, so counting screen
    pixels that EQUAL one of these is a check on what the sprite path
    painted -- no tolerance for a background or a blend to satisfy.
    """
    im = Image.open(os.path.join(CURSOR_DATA, theme, "arrow.qoi")).convert("RGBA")
    px = im.load()
    return {px[x, y][:3] for y in range(im.height) for x in range(im.width)
            if px[x, y][3] == 255}


def colour_hits(qmp, tag, colours):
    path = os.path.join(tempfile.gettempdir(), f"cursor_{tag}.png")
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    return sum(1 for y in range(PARK_Y - 8, PARK_Y + 80)
               for x in range(PARK_X - 8, PARK_X + 80)
               if im.getpixel((x, y)) in colours)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "cursor_theme_test")

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
    want = expected_shapes()
    theme, n = loaded_count(dbg)
    check("the default theme loads every shape", n == want,
          f'theme="{theme}" loaded={n} of {want}')

    # --- it is what is actually drawn ---------------------------------
    DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
    time.sleep(0.5)
    ink_default = cursor_ink(qmp, "default")
    check("the pointer is drawn", ink_default > 40, f"{ink_default} px")

    # --- a different THEME changes the drawn shape --------------------
    set_setting(dbg, "cursor_theme", "bold")
    theme, n = loaded_count(dbg)
    check("the bold theme loads every shape", n == want,
          f'theme="{theme}" loaded={n} of {want}')

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

    # --- every IMAGE theme loads, and paints its OWN colours ----------
    #
    # Only the SOFTWARE sprite is in a screendump -- the hardware plane
    # is not (docs/conventions/gui.md) -- so this needs a guest whose
    # display has no cursor plane, and says so rather than passing.
    state = dbg.state() or {}
    hw = bool(state.get("hwcursor"))
    check("the pointer is the software sprite (pixel checks are meaningful)",
          not hw, "hwcursor on -- boot with a VGA that has no cursor plane" if hw else "")
    themes = image_themes()
    check("image themes are installed", len(themes) >= 1, ", ".join(themes))
    # The CONTROL first: the mask default must show none of an image
    # theme's colours, or a hit below proves nothing.
    DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
    time.sleep(0.5)
    for t in themes:
        stray = colour_hits(qmp, f"control_{t}", own_colours(t) - {(0, 0, 0), (255, 255, 255)})
        check(f"control: the default pointer shows none of {t}'s colours", stray == 0,
              f"{stray} px")
    for t in themes:
        set_setting(dbg, "cursor_theme", t)
        theme, n = loaded_count(dbg)
        check(f"the {t} theme loads every shape", theme == t and n == want,
              f'theme="{theme}" loaded={n} of {want}')
        DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
        time.sleep(0.5)
        hits = colour_hits(qmp, f"img_{t}", own_colours(t) - {(0, 0, 0), (255, 255, 255)})
        check(f"the {t} pointer paints its own colours", hits >= 20, f"{hits} px")
        # large reloads the 2x RENDERING; its ink grows about fourfold
        ink1 = cursor_ink(qmp, f"img1_{t}")
        set_setting(dbg, "cursor_size", "large")
        DebugConsole.warp_cursor(dbg, qmp, PARK_X, PARK_Y)
        time.sleep(0.5)
        ink2 = cursor_ink(qmp, f"img2_{t}")
        r = ink2 / max(1, ink1)
        check(f"{t} at large is about four times the area",
              3.0 <= r <= 5.0, f"normal={ink1} large={ink2} ratio={r:.2f}")
        # ...and it is the 2x FILE, not the 1x one doubled -- the area
        # cannot tell those apart, the loader's own report can.
        rep = [line for line in dbg.logs() if "rendered for" in line]
        m = re.search(r"(\d+) of (\d+) images rendered for 2x", rep[-1]) if rep else None
        check(f"{t} at large loaded its 2x renderings",
              bool(m) and m.group(1) == m.group(2) and int(m.group(1)) == want,
              rep[-1].strip() if rep else "no report")
        set_setting(dbg, "cursor_size", "normal")
    bad = [line for line in dbg.logs() if "did not decode" in line or "-- refused" in line]
    check("no image file failed to decode or was refused", not bad, bad[0] if bad else "")
    set_setting(dbg, "cursor_theme", "default")

    # --- a theme nobody installed is REFUSED --------------------------
    #
    # The registry is the gate for an enum's value now: a setting that
    # declares its choices refuses one that is not among them
    # (api/setting.h). So the state this used to check -- the setting
    # holding a name with no theme behind it, and the compositor left
    # with no shapes at all -- is no longer reachable through `config`,
    # which is a better floor than asserting how gracefully it failed.
    before_theme, before_n = loaded_count(dbg)
    dbg.send("gui spawn /bin/config set cursor_theme broken")
    time.sleep(1.0)
    theme, n = loaded_count(dbg)
    # NO NEW REPORT IS THE PASS. The compositor logs a line when it
    # reloads a theme, so a refused change produces nothing at all --
    # `loaded_count()` reads the lines since the last drain and
    # correctly finds none. A theme name coming back here would mean it
    # reloaded, which is the failure.
    check("a theme that does not exist is refused, not applied",
          theme is None or (theme == before_theme and n == before_n),
          f'theme="{theme}" loaded={n}, was "{before_theme}"/{before_n}')
    # ...and the value on disk is still the working one, read through a
    # path the compositor is not on.
    out = dbg.send("sh cat /etc/toyos.conf") or ""
    check("...and /etc still names a theme that exists",
          "cursor_theme=broken" not in out, out[-200:])

    DebugConsole.warp_cursor(dbg, qmp, PARK_X + 40, PARK_Y + 40)
    time.sleep(0.5)
    path = os.path.join(tempfile.gettempdir(), "cursor_broken.png")
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    ink_fallback = sum(1 for y in range(PARK_Y + 32, PARK_Y + 80)
                        for x in range(PARK_X + 32, PARK_X + 80)
                        if im.getpixel((x, y)) != BG)
    # The pointer is still drawn after a refused change -- the check that
    # a rejected setting costs nothing, rather than the old one that a
    # missing theme costs only its own shapes.
    check("the pointer still draws after a refused theme",
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
