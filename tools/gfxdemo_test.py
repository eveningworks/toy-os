#!/usr/bin/env python3
"""tools/gfxdemo_test.py -- drive the Shapes demo and assert it really draws.

WHAT THIS IS
------------
Shapes (`userland/gfxdemo.c`) is a RING-3 client that draws with the
shared geometry module -- `kernel/lib/geom.c` and `fixed.c`, compiled a
second time for userland. This drives it and checks three claims that a
screenshot cannot settle on its own:

  1. It is genuinely a ring-3 client, not a kernel-space window that
     happens to look like one (`gui windows --json`'s `client_pid`).
  2. It is actually ANIMATING -- and, the other half of that, that it
     stops dead at speed 0. "The frame changed" alone proves very
     little; a blinking caret would satisfy it. The pair does not.
  3. The anti-aliasing toggle changes the RASTERISER, not just a
     checkbox: an AA frame contains many more distinct colours than an
     aliased one, because partial coverage is what AA emits.

    python3 tools/vm.py start          # or --disk a copy
    python3 tools/gfxdemo_test.py      # enters GUI mode itself
    echo $?                            # 0 = every check passed

WHY IT IS A TOOL AND NOT A SCRIPT
---------------------------------
Same reasoning as tools/uidemo_test.py, plus one trap of its own:

  * **Geometry comes from the app.** Shapes logs `gfxdemo: layout canvas
    <x> <y> <w> <h>` at startup; this reads that rather than re-deriving
    the rect from font metrics in Python. That copy drifts silently the
    first time the layout changes -- the exact failure uidemo_test.py
    documents.
  * **The canvas region must be sampled WITHOUT the cursor in it.** The
    mouse sprite is composited on top, so a cursor parked over the
    canvas turns "did the drawing change" into "did the cursor move".
    park_cursor() puts it somewhere harmless first.
  * **Shapes polls rather than blocking**, so it keeps drawing while
    this script talks over serial. Anything comparing two frames has to
    set speed 0 first and then let the loop settle, or the "static"
    frame is still mid-rotation.

CAVEAT
------
Injected input enters below the PS/2 driver (see tools/gui_debug.py), so
a clean run says nothing about the real mouse or keyboard path.
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"

# The demo's own key bindings (userland/gfxdemo.c). Sent as hex because
# `gui key` splits its arguments on whitespace and parses ints that way.
K_A = "0x61"        # toggle anti-aliasing
K_MINUS = "0x2d"    # slower
K_PLUS = "0x2b"     # faster
K_Q = "0x71"        # quit


class Shapes:
    def __init__(self, dbg, qmp, verbose=False):
        self.dbg = dbg
        self.qmp = qmp
        self.verbose = verbose
        self.fails, self.passes = [], []
        self.win = None
        self.canvas = None      # (x, y, w, h), content-relative

    # -- plumbing ------------------------------------------------------

    def events(self):
        return self.dbg.logs("gfxdemo:", clear=True)

    def key(self, k):
        self.dbg.send(f"gui key {k}")
        self.dbg.settle()
        return self.events()

    def click_content(self, cx, cy):
        x = self.win["content"]["x"] + cx
        y = self.win["content"]["y"] + cy
        self.dbg.send(f"gui click {x} {y}")
        self.dbg.settle()
        return self.events()

    def park_cursor(self):
        """Get the mouse sprite out of the canvas before sampling pixels.

        The cursor is composited over the window, so a frame comparison
        with it parked on the canvas measures the cursor, not the
        drawing. Bottom-left of the screen is always desktop here.
        """
        self.dbg.warp_cursor(self.qmp, 40, 620)
        self.dbg.settle()

    # -- assertions ----------------------------------------------------

    def check(self, name, ok, why=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        if ok:
            self.passes.append(name)
        else:
            if why:
                print(f"        {why}")
            self.fails.append(name)

    def check_log(self, name, got, want):
        self.check(name, any(want in l for l in got),
                   f"wanted {want!r}, got {got}")

    # -- pixels --------------------------------------------------------

    def canvas_image(self, tag):
        """Screenshot, cropped to the canvas. Returns a PIL image."""
        from PIL import Image
        path = os.path.join(tempfile.gettempdir(), f"gfxdemo-{tag}.png")
        self.qmp.screenshot(os.path.abspath(path))
        img = Image.open(path).convert("RGB")
        cx, cy, cw, ch = self.canvas
        x0 = self.win["content"]["x"] + cx
        y0 = self.win["content"]["y"] + cy
        # Inset by the border so the canvas frame itself, which never
        # changes, does not dilute either measurement.
        return img.crop((x0 + 2, y0 + 2, x0 + cw - 2, y0 + ch - 2))

    @staticmethod
    def differing_fraction(a, b):
        pa, pb = a.load(), b.load()
        w, h = a.size
        diff = sum(1 for y in range(0, h, 2) for x in range(0, w, 2)
                   if pa[x, y] != pb[x, y])
        return diff / float((w // 2 + 1) * (h // 2 + 1))

    @staticmethod
    def distinct_colors(img):
        # getcolors() needs a ceiling high enough not to give up and
        # return None -- 24-bit colour, so this is "no ceiling".
        return len(img.getcolors(1 << 24) or [])

    # -- setup ---------------------------------------------------------

    def open(self):
        """Launch Shapes from a Terminal and read its self-reported layout."""
        self.dbg.send("gui open Terminal")
        self.dbg.settle()
        for ch in "run shapes":
            self.dbg.send(f"gui key {'0x20' if ch == ' ' else ch}")
        self.dbg.settle()
        self.dbg.send("gui key 0x0d")

        # The ELF has to be loaded and the window created before any of
        # this is answerable; poll rather than guessing at a sleep.
        deadline = time.time() + 15
        lines = []
        while time.time() < deadline:
            lines += self.dbg.logs("gfxdemo:", clear=True)
            if any("layout canvas" in l for l in lines):
                break
            time.sleep(0.3)
        self.startup = lines

        for l in lines:
            if "layout canvas" in l:
                parts = l.split("layout canvas")[1].split()
                self.canvas = tuple(int(p) for p in parts[:4])
        self.win = self.dbg.window("Shapes")
        if self.win is None:
            print("gfxdemo_test: no Shapes window -- did `run shapes` fail?")
            print("  startup log:", lines)
            sys.exit(2)
        if self.canvas is None:
            print("gfxdemo_test: Shapes never logged its layout:", lines)
            sys.exit(2)

    def set_speed(self, target):
        """Walk the speed to `target` with the keyboard, confirming as it
        goes -- the app logs `gfxdemo: speed N` on every change, so this
        does not have to assume a keystroke landed."""
        for _ in range(60):
            got = self.key(K_MINUS if target == 0 else K_PLUS)
            for l in got:
                if "speed" in l:
                    try:
                        if int(l.split("speed")[1].split()[0]) == target:
                            return True
                    except (ValueError, IndexError):
                        pass
            if target == 0 and not got:
                return True     # already at 0: nothing left to log
        return False


def run(d):
    print("gfxdemo_test: checks")

    # 1. It came up, and it is a ring-3 client.
    d.check_log("app announced itself", d.startup, "gfxdemo: ready")
    d.check_log("anti-aliasing starts on", d.startup, "gfxdemo: aa on")
    pid = d.win.get("client_pid", 0)
    d.check("window is a ring-3 client, not kernel-space", pid > 0,
            f"client_pid was {pid} -- 0 means the WM drew it in ring 0")

    d.park_cursor()

    # 2. It animates -- and stops when told to. Neither half means much
    #    alone; a frame that always changes could be a blinking caret,
    #    and one that never changes could be an app that died.
    a = d.canvas_image("spin-a")
    time.sleep(0.7)
    b = d.canvas_image("spin-b")
    moved = d.differing_fraction(a, b)
    d.check("the shapes are rotating", moved > 0.01,
            f"only {moved:.4%} of sampled pixels changed between frames")

    d.check("speed reaches 0 from the keyboard", d.set_speed(0))
    time.sleep(0.5)
    s1 = d.canvas_image("still-a")
    time.sleep(0.7)
    s2 = d.canvas_image("still-b")
    still = d.differing_fraction(s1, s2)
    d.check("at speed 0 the frame is static", still < 0.001,
            f"{still:.4%} of sampled pixels still changed at speed 0")

    # 3. The AA toggle reaches the rasteriser. Held at speed 0 so the
    #    only difference between the two frames is the toggle.
    aa_on = d.distinct_colors(s2)
    got = d.key(K_A)
    d.check_log("pressing A turns anti-aliasing off", got, "gfxdemo: aa off")
    time.sleep(0.4)
    aliased = d.canvas_image("aliased")
    aa_off = d.distinct_colors(aliased)
    # Report the numbers either way: a future session changing the
    # palette wants to see how much margin this check actually has.
    print(f"        ({aa_on} distinct colours with AA, {aa_off} without)")
    d.check("anti-aliasing actually changes the rasteriser",
            aa_on > aa_off * 2,
            f"{aa_on} distinct colours with AA vs {aa_off} without -- "
            "a real AA path emits partial coverage, so many more")

    got = d.key(K_A)
    d.check_log("pressing A again turns it back on", got, "gfxdemo: aa on")

    d.check("speed returns from the keyboard", d.set_speed(3))

    # 4. The buttons work, and report through the same log grammar. The
    #    button row sits one gap below the canvas (userland/gfxdemo.c's
    #    layout()), which is why the canvas rect is all this needs.
    by = d.canvas[1] + d.canvas[3] + 8 + 12    # +12: inside the row, not on its edge
    got = d.click_content(d.canvas[0] + 20, by)   # "Slower", the first button
    d.check_log("the Slower button changes speed", got, "gfxdemo: speed 2")

    # 5. It exits cleanly when asked, rather than being killed.
    got = d.key(K_Q)
    d.check_log("q exits the app", got, "gfxdemo: exiting")
    time.sleep(0.5)
    d.check("the window is gone after exit", d.dbg.window("Shapes") is None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--shot", metavar="DIR",
                    help="also write shapes-demo.png / shapes-aliased.png here")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.0)

    dbg = DebugConsole(args.sock)
    d = Shapes(dbg, qmp, verbose=args.verbose)
    d.open()
    print(f"gfxdemo_test: canvas {d.canvas}, client_pid {d.win.get('client_pid')}")

    if args.shot:
        os.makedirs(args.shot, exist_ok=True)
        qmp.screenshot(os.path.abspath(os.path.join(args.shot, "shapes-demo.png")))

    run(d)

    print(f"\ngfxdemo_test: {len(d.passes)} passed, {len(d.fails)} failed")
    for f in d.fails:
        print("  FAILED:", f)
    dbg.close()
    return 1 if d.fails else 0


if __name__ == "__main__":
    sys.exit(main())
