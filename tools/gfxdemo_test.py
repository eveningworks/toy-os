#!/usr/bin/env python3
"""tools/gfxdemo_test.py -- drive the Shapes demo and assert it really draws.

WHAT THIS IS
------------
Shapes (`userland/gui/gfxdemo.c`) is a RING-3 client that draws with the
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
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"

# The demo's own key bindings (userland/gui/gfxdemo.c). Sent as hex because
# `gui key` splits its arguments on whitespace and parses ints that way.
K_A = "0x61"        # toggle anti-aliasing
K_S = "0x73"        # toggle the 2D / 3D scene
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
        self.buttons = None     # (x, y, w, h, pitch, count), ditto

    # -- plumbing ------------------------------------------------------

    def events(self):
        return self.dbg.logs("gfxdemo:", clear=True)

    def key(self, k):
        self.dbg.send(f"gui key {k}")
        self.dbg.settle()
        return self.events()

    def key_until(self, k, want, timeout=6.0):
        """key(), waiting for the line the app logs in response. See
        click_content_until() for why one read is not enough."""
        got = self.key(k)
        deadline = time.time() + timeout
        while not any(want in l for l in got) and time.time() < deadline:
            time.sleep(0.2)
            got += self.events()
        return got

    def click_content(self, cx, cy):
        x = self.win["content"]["x"] + cx
        y = self.win["content"]["y"] + cy
        self.dbg.send(f"gui click {x} {y}")
        self.dbg.settle()
        return self.events()

    def click_content_until(self, cx, cy, want, timeout=6.0):
        """Click, then WAIT for the line the app logs in response.

        One read after dbg.settle() is a poll whose exit condition is
        weaker than what the caller needs: settle() knows the debug
        console has gone quiet, not that this client has handled the
        click and logged. Under parallel load that gap is wide enough to
        lose the line -- `the 2D / 3D button switches back` failed that
        way with four other guests running and passed alone, which reads
        exactly like a broken toggle.

        Accumulates, because events() CLEARS as it reads: a line that
        arrived in an earlier poll would otherwise be dropped by the
        next one.
        """
        got = self.click_content(cx, cy)
        deadline = time.time() + timeout
        while not any(want in l for l in got) and time.time() < deadline:
            time.sleep(0.2)
            got += self.events()
        return got

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
        self.qmp.screenshot(os.path.abspath(path), stable=False)  # the demo animates
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
        """Launch Shapes and read its self-reported layout.

        Spawned directly rather than typed at a Terminal: the
        kernel-space Terminal retired in M41's stage 0, and the ring-3
        one has no window yet when the injected keys would arrive.
        """
        self.dbg.send("gui spawn /bin/wm/demos/shapes")

        # The ELF has to be loaded and the window created before any of
        # this is answerable; poll rather than guessing at a sleep.
        #
        # WAIT FOR BOTH LINES, not just the first. This used to break on
        # `layout canvas` and then require `layout buttons` immediately
        # after -- a poll whose exit condition is weaker than what the
        # code following it needs, which is a flake by construction. It
        # failed roughly one run in three under `gui_regress`'s parallel
        # load and never once when the tool ran alone, which is how a
        # timing bug in a HARNESS disguises itself as a regression in
        # whatever happened to be built that day. Same bug, same fix, as
        # calculator_client_test.py's (see CLAUDE.md).
        deadline = time.time() + 15
        lines = []
        while time.time() < deadline:
            lines += self.dbg.logs("gfxdemo:", clear=True)
            if (any("layout canvas" in l for l in lines)
                    and any("layout buttons" in l for l in lines)):
                break
            time.sleep(0.3)
        self.startup = lines

        for l in lines:
            if "layout canvas" in l:
                parts = l.split("layout canvas")[1].split()
                self.canvas = tuple(int(p) for p in parts[:4])
            if "layout buttons" in l:
                # x y w h pitch count -- so a click lands on the button
                # the test MEANT, at any font size. Replaces this file's
                # old `canvas_y + canvas_h + 8 + 12`, which re-derived
                # the app's spacing here and would have been wrong the
                # moment a fourth button was added.
                parts = l.split("layout buttons")[1].split()
                self.buttons = tuple(int(p) for p in parts[:6])
        self.win = self.dbg.window("Shapes")
        if self.win is None:
            print("gfxdemo_test: no Shapes window -- did `run shapes` fail?")
            print("  startup log:", lines)
            sys.exit(2)
        if self.canvas is None or self.buttons is None:
            print("gfxdemo_test: Shapes never logged its layout:", lines)
            sys.exit(2)

    def click_button(self, index):
        """Click the centre of button `index`, from the row the app
        reported. Never from arithmetic over the canvas rect."""
        bx, by, bw, bh, pitch, count = self.buttons
        if index >= count:
            raise IndexError(f"button {index} of {count}")
        return self.click_content(bx + index * pitch + bw // 2, by + bh // 2)

    def click_button_until(self, index, want, timeout=6.0):
        """click_button(), waiting for the log line it should produce."""
        bx, by, bw, bh, pitch, count = self.buttons
        if index >= count:
            raise IndexError(f"button {index} of {count}")
        return self.click_content_until(bx + index * pitch + bw // 2,
                                        by + bh // 2, want, timeout)

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


def check_cube(d):
    """The 3D scene: a wireframe cube from geom_transform3().

    What is actually worth asserting here is narrow, because the
    projection maths is already pinned precisely by KTESTs in
    kernel/lib/geom_test.c (the rotation axes, near-bigger-than-far, the
    clamp at the eye). Re-testing arithmetic through a screenshot would
    be a worse version of a test that already exists. What only THIS can
    check is that the app wired it up:

      * the scene toggles, from the key and from the button;
      * the cube animates, and stops dead -- the same pairing the 2D
        scene gets, because either half alone is satisfied by a bug;
      * the edges are DEPTH-SHADED, which is the one visible property
        that cannot exist without a real 3D transform. A flat wireframe
        drawn with 2D rotations has one edge colour; a projected one has
        a different shade per edge. Counted with AA OFF, or
        anti-aliasing's partial coverage would supply the extra colours
        by itself and the check would pass on a flat drawing.
      * toggling away and back restores the 2D scene EXACTLY, which
        catches a scene switch that leaves state behind.
    """
    d.check("speed reaches 0 before comparing frames", d.set_speed(0))
    time.sleep(0.5)

    # AA off first: the colour count below has to measure shading, not
    # coverage. (Left off for both scenes, so the comparison is fair.)
    d.key(K_A)
    time.sleep(0.4)
    flat_2d = d.canvas_image("scene-2d")
    colors_2d = d.distinct_colors(flat_2d)

    got = d.key_until(K_S, "gfxdemo: scene 3d")
    d.check_log("pressing S switches to the 3D scene", got, "gfxdemo: scene 3d")
    time.sleep(0.5)
    cube = d.canvas_image("scene-3d")

    d.check("the 3D scene draws something different",
            d.differing_fraction(flat_2d, cube) > 0.01,
            "the canvas barely changed when the scene switched")

    colors_3d = d.distinct_colors(cube)
    print(f"        ({colors_2d} distinct colours in 2D aliased, {colors_3d} in 3D)")
    d.check("the cube's edges are shaded by depth",
            colors_3d > colors_2d,
            f"{colors_3d} distinct colours in the 3D scene vs {colors_2d} in the "
            "2D one, both aliased -- a per-edge depth shade is the only thing "
            "that can add them, and it needs a real projection to exist")

    # Round trip, taken NOW rather than at the end of this function: the
    # claim is "the same angle draws the same pixels", so nothing between
    # the two captures may advance the angle. The first draft ran the
    # spin checks in between and failed here for exactly that reason --
    # the assertion was wrong, not the app.
    got = d.click_button_until(3, "gfxdemo: scene 2d")  # the BUTTON, not
                               # the key, so both paths into the toggle
                               # are covered
    d.check_log("the 2D / 3D button switches back", got, "gfxdemo: scene 2d")
    time.sleep(0.5)
    back = d.canvas_image("scene-2d-again")
    d.check("returning to the 2D scene restores it exactly",
            d.differing_fraction(flat_2d, back) == 0.0,
            "the 2D scene came back different from how it was left -- at "
            "speed 0 with the angle unchanged it must be pixel-identical")

    # Now the animation pair, which needs the angle to move.
    d.key(K_S)
    time.sleep(0.3)
    d.check("speed returns for the cube", d.set_speed(4))
    a = d.canvas_image("cube-spin-a")
    time.sleep(0.7)
    b = d.canvas_image("cube-spin-b")
    d.check("the cube is rotating", d.differing_fraction(a, b) > 0.01,
            "the cube did not move between frames")

    d.check("the cube stops at speed 0", d.set_speed(0))
    time.sleep(0.5)
    s1 = d.canvas_image("cube-still-a")
    time.sleep(0.7)
    s2 = d.canvas_image("cube-still-b")
    still = d.differing_fraction(s1, s2)
    d.check("at speed 0 the cube is static", still < 0.001,
            f"{still:.4%} of sampled pixels still changed at speed 0")

    # Hand the rest of the run the state it expects: 2D scene, AA on,
    # speed 3. A check that leaves global state behind fails the NEXT
    # check instead of itself, which is a genuinely confusing way to
    # debug -- it happened on this function's first run.
    d.key(K_S)
    d.set_speed(3)
    d.key(K_A)   # anti-aliasing back on, as the rest of the run expects


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

    check_cube(d)

    # 5. The buttons work, and report through the same log grammar.
    #    Clicked at the centre the APP reported, not at an offset derived
    #    here -- see click_button().
    got = d.click_button_until(0, "gfxdemo: speed 2")   # "Slower"
    d.check_log("the Slower button changes speed", got, "gfxdemo: speed 2")

    # 5. It exits cleanly when asked, rather than being killed.
    got = d.key_until(K_Q, "gfxdemo: exiting")
    d.check_log("q exits the app", got, "gfxdemo: exiting")
    time.sleep(0.5)
    d.check("the window is gone after exit", d.dbg.window("Shapes") is None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--shot", metavar="DIR",
                    help="also write shapes-demo.png / shapes-aliased.png here")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "gfxdemo_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

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
