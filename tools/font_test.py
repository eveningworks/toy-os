#!/usr/bin/env python3
"""Runtime fonts: loading a TTF from disk, switching faces live, and
proportional advance widths actually reaching the screen.

The glyphs the desktop draws with come from one of two places: the
tables tools/genttf.py baked into the kernel image, or a .ttf under
/usr/share/fonts rasterized at runtime (kernel/lib/ttf.c). Switching
between them is a setting, and WIN_EV_FONT tells every client its cached
metrics went stale.

WHAT THIS TEST IS SHAPED AROUND. "Text is on screen" proves nothing --
the baked font is a complete, working fallback, so a rasterizer that
produced garbage, or a face switch that silently did nothing, leaves a
perfectly readable desktop behind. Every check here is therefore a
DIFFERENCE between two states, and the load-bearing one is that
liberation-sans (proportional) draws the same labels NARROWER than
dejavu-sans-mono (monospace) does. A build that ignored per-glyph
advances would still render both faces, still switch between them, and
still fail that one check -- which is the point.

The measurement is the rightmost ink in the desktop's icon-label column.
It is a width, not a pixel count: a face with heavier stems has more ink
at the same width, so counting ink would mostly measure boldness.

Usage (the VM must already be up):
    python3 tools/vm.py start
    python3 tools/font_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
MONO = "dejavu-sans-mono"
PROP = "liberation-sans"

# THE VERSION TEXT, bottom right: two lines of ~40 characters, drawn
# RIGHT-ALIGNED against the screen edge. That last part is what makes it
# the right ruler here -- a narrower font starts further right, so its
# LEFTMOST ink moves by the whole difference in string width, tens of
# pixels rather than the two or three the desktop icon captions would
# show (those are clipped to the icon cell, so a narrower font mostly
# just un-truncates them).
TEXT_X0, TEXT_X1 = 700, 1280
TEXT_Y0, TEXT_Y1 = 655, 690
BG = (24, 60, 90)

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def text_left(qmp, tag):
    """Leftmost inked column of the right-aligned version text, and ink.

    Uses a SETTLED frame: a face change is a client-side repaint two
    process hops away from the setting being written, and a capture
    landing mid-paint compares two half-drawn desktops.
    """
    path = os.path.join(tempfile.gettempdir(), f"font_{tag}.png")
    qmp.stable_pixels(path, box=(TEXT_X0, TEXT_Y0, TEXT_X1, TEXT_Y1))
    # stable_pixels() writes the WHOLE screen and only COMPARES the box,
    # so the crop has to happen here. Scanning the full frame instead
    # counts the taskbar and every icon as "ink" and the measurement
    # stops meaning anything -- it read 53,968 in a 20,300-pixel band.
    im = Image.open(path).convert("RGB").crop((TEXT_X0, TEXT_Y0, TEXT_X1, TEXT_Y1))
    w, h = im.size
    left, ink = w, 0
    for y in range(h):
        for x in range(w):
            if im.getpixel((x, y)) != BG:
                ink += 1
                if x < left:
                    left = x
    return TEXT_X0 + left, ink


def set_face(dbg, name):
    dbg.send(f"gui spawn /bin/config set system.font_face {name}")
    time.sleep(1.2)


def set_size(dbg, px):
    dbg.send(f"gui spawn /bin/config set system.font_size {px}")
    time.sleep(1.2)


def wait_log(dbg, needle, timeout=8.0):
    """The most recent COMPOSITOR log line containing `needle`, waited for.

    Note what this can and cannot see: DebugConsole.logs() is the
    compositor's own buffer, so `wm: font changed -- WxH cell` is here
    and the kernel's `font: <face> at <n>px` line (which goes to klog)
    is NOT. That turns out to be the better source anyway -- the cell the
    CLIENT ended up with is the thing under test, and reading the
    kernel's intention would prove one hop less.

    A POLL, not a sleep. The setting is written by a short-lived ring-3
    process, applied in the kernel and logged there, and the client
    repaint that follows is a third hop -- so "how long does that take"
    is a load-dependent question with no good fixed answer. Reads
    without clearing so several waits can look at the same buffer.
    """
    deadline = time.time() + timeout
    while True:
        lines = [l for l in dbg.logs(clear=False) if needle in l]
        if lines:
            return lines[-1].strip()
        if time.time() > deadline:
            return ""
        time.sleep(0.3)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.5)

    dbg = DebugConsole(args.sock)
    print("runtime fonts")

    # --- the faces are on disk and one of them is in use ---------------
    # ESTABLISH the starting state rather than inherit it: font_face and
    # font_size both persist to /etc/toyos.conf, which survives a build,
    # so a previous run leaves its choice behind and every measurement
    # below is relative to a baseline that would otherwise be silently
    # wrong. Via PROP first so the switch to MONO is a real change --
    # setting a setting to the value it already holds does nothing, by
    # design, and would leave nothing to observe.
    set_size(dbg, 14)
    set_face(dbg, PROP)
    dbg.logs()  # drain, so every wait below sees only what IT caused
    set_face(dbg, MONO)
    mono_cell = wait_log(dbg, "wm: font changed")
    check("a face rasterizes from /usr/share/fonts and reaches the compositor",
          "cell" in mono_cell, mono_cell)

    mono_left, mono_ink = text_left(qmp, "mono")
    check("the desktop draws text with it", mono_ink > 200,
          f"left={mono_left} ink={mono_ink}")

    # --- a live face switch reaches the screen -------------------------
    # No restart: WIN_EV_FONT tells the compositor its metrics moved.
    dbg.logs()
    set_face(dbg, PROP)
    prop_cell = wait_log(dbg, "wm: font changed")
    check("switching face tells the compositor a new cell", "cell" in prop_cell,
          prop_cell)
    check("the two faces do not have the same cell", prop_cell != mono_cell,
          f"{mono_cell!r} -> {prop_cell!r}")

    prop_left, prop_ink = text_left(qmp, "prop")
    check("the switch reached the screen without a restart",
          prop_left != mono_left or abs(prop_ink - mono_ink) > 100,
          f"left {mono_left} -> {prop_left}, ink {mono_ink} -> {prop_ink}")

    # THE CHECK THIS TOOL EXISTS FOR. A proportional face must draw the
    # same captions in less width than a monospace one -- that is what
    # per-glyph advances DO. Ignore hmtx and every other check here still
    # passes.
    check("proportional text is narrower than monospace",
          prop_left > mono_left + 4,
          f"right-aligned text starts at {prop_left} vs {mono_left}")

    # --- an arbitrary size, which is only possible with a rasterizer ---
    dbg.logs()
    set_size(dbg, 13)
    size13 = wait_log(dbg, "wm: font changed")
    check("a size nobody baked is rasterized", "cell" in size13 and size13 != prop_cell,
          f"{prop_cell!r} -> {size13!r}")

    # --- and the baked font is still there -----------------------------
    set_size(dbg, 14)
    set_face(dbg, "builtin")
    time.sleep(1.0)
    builtin_left, builtin_ink = text_left(qmp, "builtin")
    check("the baked font still draws when no face is selected",
          builtin_ink > 200, f"left={builtin_left} ink={builtin_ink}")
    check("the baked font is not the face we just left",
          builtin_left != prop_left or abs(builtin_ink - prop_ink) > 100,
          f"left {prop_left} -> {builtin_left}")

    # Put the machine back the way a fresh image boots.
    set_face(dbg, MONO)
    set_size(dbg, 14)

    passed = sum(1 for _, ok in checks if ok)
    failed = len(checks) - passed
    print(f"font_test: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
