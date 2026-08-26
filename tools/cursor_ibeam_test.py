#!/usr/bin/env python3
"""The I-beam: a client naming its pointer shape, and the compositor's clamp.

A ring-3 app says "text goes here" (abi/win_proto.h's WIN_REQ_CURSOR) and
the compositor draws the theme's `text` shape while the pointer is inside
that window's CONTENT area. This drives all four ways a shape gets named
-- a widget's ops table, an app's own uapp_set_cursor(), a window-wide
constant, and the WM's own modal chrome -- and both halves of the clamp.

WHAT MAKES THIS MEASURE ANYTHING, because "a cursor is on screen" is
satisfied by every possible bug here: the two shapes are told apart by
WHERE THEY SIT RELATIVE TO THE HOTSPOT, not by size or by ink. The arrow
is drawn from the pointer DOWN AND RIGHT (its bbox starts at 0,0); the
I-beam is CENTRED on it (its bbox starts around -3,-8). So a wrong shape
cannot pass as the right one, and neither can a missing one -- a probe
that finds no sprite at all fails both tests rather than one.

THE SPRITE IS ISOLATED BY A THREE-FRAME DIFF, not a two-frame one. Park
away, snapshot; warp to the point, snapshot; park away again, snapshot;
keep only pixels that differ from BOTH away-frames. A one-off repaint
(a hover highlight clearing, the tray clock ticking, a caret blinking)
lands in one comparison and is filtered out; the sprite is in both. The
two-frame version reported Notepad's menu bar as an I-beam, because the
menu title under the pointer redrew its hover.

Usage (the VM must already be up):
    python3 tools/vm.py start
    python3 tools/cursor_ibeam_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402

DEFAULT_SOCK = ".vm.serial"

# Somewhere with nothing to hover and nothing that animates: clear of the
# desktop icon column, of every window this tool opens, of the taskbar
# and of the version text in the bottom-right corner.
PARK = (900, 520)
# The window the diff is taken over. Generous enough for the largest
# shape at 3x scale, so a cursor_size setting left behind by another tool
# cannot push ink outside what is being read.
HALF = 40

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


class Prober:
    def __init__(self, dbg, qmp, tmp):
        self.dbg, self.qmp, self.tmp = dbg, qmp, tmp
        self.n = 0

    def _shot(self, tag):
        p = os.path.join(self.tmp, f"{tag}.png")
        self.qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    def sprite(self, x, y):
        """The cursor sprite's bounding box at (x, y), relative to (x, y).

        Returns (dx0, dy0, dx1, dy1, npixels), or None if nothing there
        differs -- which is itself a finding, not an error.
        """
        self.n += 1
        self.dbg.warp_cursor(self.qmp, *PARK)
        self.dbg.settle()
        away1 = self._shot(f"p{self.n}_away1")
        self.dbg.warp_cursor(self.qmp, x, y)
        self.dbg.settle()
        here = self._shot(f"p{self.n}_here")
        self.dbg.warp_cursor(self.qmp, *PARK)
        self.dbg.settle()
        away2 = self._shot(f"p{self.n}_away2")

        w, h = here.size
        pts = []
        for yy in range(max(0, y - HALF), min(h, y + HALF)):
            for xx in range(max(0, x - HALF), min(w, x + HALF)):
                px = here.getpixel((xx, yy))
                # In BOTH comparisons -- see the module docstring.
                if px != away1.getpixel((xx, yy)) and px != away2.getpixel((xx, yy)):
                    pts.append((xx - x, yy - y))
        if not pts:
            return None
        return (min(p[0] for p in pts), min(p[1] for p in pts),
                max(p[0] for p in pts), max(p[1] for p in pts), len(pts))


def is_ibeam(b):
    """Centred on the hotspot and narrow: a bar, not a wedge."""
    if not b:
        return False
    dx0, dy0, dx1, dy1, n = b
    return dy0 <= -4 and dx0 <= -1 and dx1 <= 4 and n >= 10


def is_arrow(b):
    """Drawn from the hotspot down and right, and nothing above or left."""
    if not b:
        return False
    dx0, dy0, dx1, dy1, n = b
    return dy0 >= 0 and dx0 >= 0 and dy1 >= 10 and n >= 40


def shape(b):
    if b is None:
        return "nothing"
    if is_ibeam(b):
        return f"I-beam {b}"
    if is_arrow(b):
        return f"arrow {b}"
    return f"neither {b}"


def close_all(dbg):
    while True:
        ws = dbg.json("gui windows --json")["windows"]
        if not ws:
            return
        dbg.send(f"gui close {len(ws) - 1}")
        dbg.settle()


def read_layout(dbg, title, path, timeout=15.0):
    """Spawn an app and collect its uapp_log_layout() rects."""
    dbg.logs("", clear=True)
    dbg.spawn(path, title)
    lines = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines += dbg.logs()
        if any(" layout " in l for l in lines):
            time.sleep(0.5)
            lines += dbg.logs()
            break
        time.sleep(0.2)
    rects = {}
    for line in lines:
        p = line.split()
        if len(p) >= 7 and p[1] == "layout":
            try:
                rects[p[2]] = tuple(int(v) for v in p[3:7])
            except ValueError:
                pass
    win = [w for w in dbg.json("gui windows --json")["windows"]
           if w["title"] == title][-1]
    return win, rects


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
    tmp = tempfile.mkdtemp(prefix="ibeam_")
    pr = Prober(dbg, qmp, tmp)

    # ESTABLISH the cursor size rather than inherit it: it persists to
    # /etc/toyos.conf, which a build does not re-seed, so a previous
    # cursor_theme_test.py run leaves `large` behind and every bbox here
    # is then measured against a shape three times the size.
    dbg.send("sh config set cursor_size normal")
    dbg.send("sh config set cursor_theme default")
    dbg.send("sh config set desktop.layout_log on")
    dbg.settle()

    print("the I-beam over text, and the arrow everywhere else")

    # --- 1. the desktop is the baseline -------------------------------
    close_all(dbg)
    b = pr.sprite(700, 300)
    check("the desktop shows an arrow", is_arrow(b), shape(b))

    # --- 2. a window-wide shape: the Terminal -------------------------
    # Named ONCE from on_open, xterm-style. Its whole content area is a
    # character grid, so there is no motion tracking in that app at all.
    dbg.open_app("Terminal")
    dbg.settle()
    win = dbg.window("Terminal")
    c = win["content"]
    b = pr.sprite(c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
    check("the Terminal's grid shows an I-beam", is_ibeam(b), shape(b))

    # THE CLAMP, and it is the load-bearing half of this tool: the same
    # window, a few pixels higher, over chrome the CLIENT does not own.
    # A compositor that simply believed the client would keep the I-beam
    # here, and every check above would still pass.
    b = pr.sprite(c["x"] + c["w"] // 2, win["y"] + 10)
    check("its title bar is NOT the client's to name", is_arrow(b), shape(b))

    # And the taskbar, which is drawn OVER the windows -- so the window
    # is first DRAGGED UNDER IT (which this WM allows), or the check
    # would be asking what the pointer looks like over a taskbar with
    # nothing beneath it, which every possible bug also passes.
    bar = dbg.taskbar()
    drop = bar["y"] + bar["h"] - (win["y"] + win["h"]) + 60
    dbg.drag(c["x"] + 200, win["y"] + 8, c["x"] + 200, win["y"] + 8 + drop)
    dbg.settle()
    moved = dbg.window("Terminal")
    covers = (moved["content"]["y"] <= bar["y"] and
              moved["content"]["y"] + moved["content"]["h"] > bar["y"])
    # y a little INSIDE the strip rather than at its bottom edge: the
    # arrow is 19px tall and drawn downwards, so a probe against the last
    # rows of the screen measures a clipped sprite and reads as neither
    # shape.
    b = pr.sprite(c["x"] + 200, bar["y"] + 2)
    check("the taskbar is not the client's either, with a window under it",
          covers and is_arrow(b),
          f"content covers the strip: {covers}; {shape(b)}")
    close_all(dbg)

    # --- 3. an app's own text area: Notepad ---------------------------
    # Not a widget -- Notepad draws its document itself, which is what
    # uapp_set_cursor() exists for. The three control points around it
    # are the point: menu bar, scrollbar and status bar are all inside
    # the same content area and must all read as arrow.
    dbg.open_app("Notepad")
    dbg.settle()
    win = [w for w in dbg.json("gui windows --json")["windows"]][-1]
    c = win["content"]
    b = pr.sprite(c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
    check("Notepad's document shows an I-beam", is_ibeam(b), shape(b))

    # Far right of the menu bar: past the last title, so no hover
    # highlight is involved even before the three-frame filter.
    b = pr.sprite(c["x"] + c["w"] - 80, c["y"] + 6)
    check("its menu bar keeps the arrow",
          is_arrow(b), shape(b))
    b = pr.sprite(c["x"] + c["w"] - 6, c["y"] + c["h"] // 2)
    check("its scrollbar keeps the arrow", is_arrow(b), shape(b))
    b = pr.sprite(c["x"] + 60, c["y"] + c["h"] - 6)
    check("its status bar keeps the arrow", is_arrow(b), shape(b))

    # --- 4. a modal covering the document -----------------------------
    # Ctrl-S opens Notepad's own Save-as dialog, whose filename field is
    # the only text in reach. The document is still THERE, underneath --
    # so this fails if the app answers from the document's rect.
    dbg.send("gui key 0x13")
    dbg.settle()
    time.sleep(0.5)
    # dialog_field_rect(): x+10, y+10+char_h+8, w-20, char_h+4 with
    # x = y = 40 and w = content width - 80. Probed at its middle, which
    # is well inside it for any font size.
    fx = c["x"] + 40 + 10
    fy = c["y"] + 40 + 10 + 16 + 8 + 8
    b = pr.sprite(fx + 100, fy)
    check("the Save-as field shows an I-beam", is_ibeam(b), shape(b))
    # The dialog's own panel, below the field: same modal, no text.
    b = pr.sprite(fx + 100, c["y"] + c["h"] - 80)
    check("the dialog's panel does not", is_arrow(b), shape(b))
    dbg.send("gui key 27")  # Esc closes the dialog
    dbg.settle()
    close_all(dbg)

    # --- 5. a WIDGET declaring it, with no app code at all -------------
    # uui_textbox's ops table names the shape; UI Demo writes not one
    # line about cursors. The dropdown beside it is the control: same
    # window, same router, no `cursor` op.
    win, rects = read_layout(dbg, "UI Demo", "/bin/wm/demos/uidemo")
    c = win["content"]
    if "textbox" in rects and "dropdown" in rects:
        x, y, w, h = rects["textbox"]
        b = pr.sprite(c["x"] + x + w // 2, c["y"] + y + h // 2)
        check("a uui_textbox shows an I-beam with no app code",
              is_ibeam(b), shape(b))
        x, y, w, h = rects["dropdown"]
        b = pr.sprite(c["x"] + x + w // 2, c["y"] + y + h // 2)
        check("a widget with no cursor op keeps the arrow",
              is_arrow(b), shape(b))
    else:
        check("UI Demo reported textbox and dropdown geometry", False,
              f"got {sorted(rects)}")
    close_all(dbg)

    failed = [n for n, ok in checks if not ok]
    print(f"\n{len(checks) - len(failed)}/{len(checks)} checks passed"
          + (f" -- FAILED: {', '.join(failed)}" if failed else ""))
    print(f"frames in {tmp}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
