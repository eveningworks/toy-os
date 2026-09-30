#!/usr/bin/env python3
"""tools/gfxdemo_test.py -- drive the Shapes demo and assert it really draws.

WHAT THIS IS
------------
Shapes (`userland/gui/demos/gfxdemo.c`) is a RING-3 client that draws with the
shared geometry module -- `kernel/lib/geom.c` and `fixed.c`, compiled a
second time for userland -- and fills its solids with ui/ugfx_tex.h's
triangle. This drives it and checks claims a screenshot cannot settle on
its own:

  1. It is genuinely a ring-3 client, not a kernel-space window that
     happens to look like one (`gui windows --json`'s `client_pid`).
  2. It is actually ANIMATING -- and, the other half of that, that it
     stops dead at speed 0. "The frame changed" alone proves very
     little; a blinking caret would satisfy it. The pair does not.
  3. The anti-aliasing toggle changes the RASTERISER, not just a
     button: an AA frame contains many more distinct colours than an
     aliased one, because partial coverage is what AA emits.
  4. The cube is projected and lit; the teapot is a solid, and its
     texture reaches the pixels.
  5. The window RESIZES: the canvas takes the room, the toolbar folds
     its view toggles into the View menu when narrow and gives them
     back when wide, the menu's rows commit, and the minimum size
     keeps the scenes and the speed controls in the strip.

What it does NOT check, because tools/teapot_hostcheck.py does it far
more precisely: the mesh's geometry and winding, the depth test, and the
Gouraud interpolation.

    python3 tools/vm.py start          # or --disk a copy
    python3 tools/gfxdemo_test.py      # enters GUI mode itself
    echo $?                            # 0 = every check passed

WHY IT IS A TOOL AND NOT A SCRIPT
---------------------------------
Same reasoning as tools/uidemo_test.py, plus traps of its own:

  * **Geometry comes from the app.** Shapes logs `gfxdemo: layout canvas
    <x> <y> <w> <h>` and its toolbar's describe lines (`toolbar.button
    i`, `toolbar.more`, the open menu's `toolbar.item 0 i`) on every
    change; this reads those rather than re-deriving rects from font
    metrics in Python. That copy drifts silently the first time the
    layout changes -- the exact failure uidemo_test.py documents.
  * **A layout block is re-sent WHOLE, and only when it changes**
    (uapp.c's dedupe), always starting with `layout canvas` -- so that
    line resets what this remembers, or a `toolbar.more` from a narrow
    window would outlive the widening that removed it.
  * **The canvas region must be sampled WITHOUT the cursor in it.**
    park_cursor() puts the sprite somewhere harmless first.
  * **Shapes keeps drawing while this script talks over serial.**
    Anything comparing two frames sets speed 0 first and lets it settle.
  * **A resize is REMEMBERED per app**, so the run puts the window back
    at the size it found, or the next tool opens a 440-pixel Shapes.

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

# The demo's own key bindings (userland/gui/demos/gfxdemo.c). Sent as hex because
# `gui key` splits its arguments on whitespace and parses ints that way.
K_A = "0x61"        # anti-aliasing
K_S = "0x73"        # next scene
K_F = "0x66"        # shaded (filled, lit)
K_T = "0x74"        # textured
K_1, K_2, K_3 = "0x31", "0x32", "0x33"   # 2D, cube, teapot
K_MINUS = "0x2d"    # slower
K_PLUS = "0x2b"     # faster
K_Q = "0x71"        # quit
K_ESC = "0x1b"

# Toolbar buttons, by index in gfxdemo.c's TOOLBAR[].
B_2D, B_CUBE, B_TEAPOT, B_SLOWER, B_FASTER, B_RESET, B_AA, B_SHADE, B_TEX = \
    0, 1, 2, 4, 6, 8, 9, 10, 11
KEEP_SHOWN = 7      # gfxdemo.c: scenes and the speed controls never fold

BG = (16, 18, 24)   # the canvas background gfxdemo.c paints


class Shapes:
    def __init__(self, dbg, qmp, verbose=False):
        self.dbg = dbg
        self.qmp = qmp
        self.verbose = verbose
        self.fails, self.passes = [], []
        self.win = None
        self.lay = {}           # the latest layout block, parsed

    # -- plumbing ------------------------------------------------------

    def _parse(self, line):
        rest = line.split("gfxdemo: layout ", 1)[1].split()
        name, nums = rest[0], [int(v) for v in rest[1:]]
        if name == "canvas":
            self.lay = {}       # a block starts here -- see the docstring
        if name == "toolbar.button":
            self.lay[("button", nums[0])] = tuple(nums[1:5])
        elif name == "toolbar.item":        # level, row, rect
            self.lay[("row", nums[1])] = tuple(nums[2:6])
        elif name in ("toolbar.shown", "toolbar.open"):
            self.lay[name] = nums[0]
        else:
            self.lay[name] = tuple(nums[:4])

    def events(self):
        got = self.dbg.logs("gfxdemo:", clear=True)
        for line in got:
            if "gfxdemo: layout " in line:
                self._parse(line)
        return got

    @property
    def canvas(self):
        return self.lay.get("canvas")

    def wait_layout(self, pred, timeout=8.0):
        deadline = time.time() + timeout
        while True:
            self.events()
            if pred(self.lay) or time.time() > deadline:
                return pred(self.lay)
            time.sleep(0.2)

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
        click and logged. Accumulates, because events() CLEARS as it
        reads.
        """
        got = self.click_content(cx, cy)
        deadline = time.time() + timeout
        while not any(want in l for l in got) and time.time() < deadline:
            time.sleep(0.2)
            got += self.events()
        return got

    def click_rect_until(self, rect, want):
        x, y, w, h = rect
        return self.click_content_until(x + w // 2, y + h // 2, want)

    def click_button_until(self, index, want):
        """A toolbar button, at the rect the APP reported."""
        rect = self.lay.get(("button", index))
        if rect is None:
            return [f"(button {index} is not in the strip: {sorted(map(str, self.lay))})"]
        return self.click_rect_until(rect, want)

    def resize(self, w, h):
        self.dbg.send(f"gui resize {w} {h}")
        self.dbg.settle()
        deadline = time.time() + 6
        while time.time() < deadline:
            self.win = self.dbg.window("Shapes")
            c = self.win["content"]
            if (c["w"], c["h"]) != self.size or (c["w"], c["h"]) == (w, h):
                break
            time.sleep(0.2)
        self.size = (self.win["content"]["w"], self.win["content"]["h"])
        # The app's own report of the new size: its toolbar spans it.
        self.wait_layout(lambda l: l.get("toolbar", (0, 0, 0))[2] == self.size[0])
        return self.size

    def park_cursor(self):
        """Get the mouse sprite out of the canvas before sampling pixels.
        Bottom-left of the screen is always desktop here."""
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
        self.events()
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
        return len(img.getcolors(1 << 24) or [])

    @staticmethod
    def ink(img):
        return sum(n for n, c in (img.getcolors(1 << 24) or []) if c != BG)

    @staticmethod
    def big_colors(img, floor=1000):
        return [c for n, c in (img.getcolors(1 << 24) or []) if c != BG and n >= floor]

    @staticmethod
    def edge_ink(img):
        """Inked pixels in the outermost 3 columns and rows: a scene that
        did not scale with the canvas runs off it and leaves ink here."""
        px = img.load()
        w, h = img.size
        n = 0
        for y in range(h):
            for x in list(range(3)) + list(range(w - 3, w)):
                n += px[x, y] != BG
        for x in range(3, w - 3):
            for y in list(range(3)) + list(range(h - 3, h)):
                n += px[x, y] != BG
        return n

    # -- setup ---------------------------------------------------------

    def open(self):
        """Launch Shapes and read its self-reported layout -- WAITING for
        everything used below, not just the first line (a poll whose exit
        condition is weaker than what follows it is a flake)."""
        self.dbg.send("gui spawn /bin/wm/demos/shapes")
        deadline = time.time() + 15
        lines = []
        while time.time() < deadline:
            lines += self.events()
            if self.canvas and "toolbar.shown" in self.lay and \
                    any("gfxdemo: ready" in l for l in lines):
                break
            time.sleep(0.3)
        self.startup = lines
        self.win = self.dbg.window("Shapes")
        if self.win is None:
            print("gfxdemo_test: no Shapes window -- did the spawn fail?")
            print("  startup log:", lines)
            sys.exit(2)
        if self.canvas is None or "toolbar.shown" not in self.lay:
            print("gfxdemo_test: Shapes never logged its layout:", lines)
            sys.exit(2)
        self.size = (self.win["content"]["w"], self.win["content"]["h"])
        self.home = self.size

    def set_speed(self, target):
        """Walk the speed to `target` with the keyboard, confirming as it
        goes -- the app logs `gfxdemo: speed N` on every change."""
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
    """The cube: from geom_transform3(), wired up by the app.

    The projection maths is pinned by KTESTs in kernel/lib/geom_test.c;
    what only THIS can check is the wiring:

      * the scene switches, from the key and from the toolbar;
      * the edges are DEPTH-SHADED, the one visible property that cannot
        exist without a real 3D transform (counted with AA OFF, or
        partial coverage supplies the extra colours by itself);
      * a shaded cube is a lit solid, and switching back is exact;
      * switching away and back restores the 2D scene EXACTLY.
    """
    d.check("speed reaches 0 before comparing frames", d.set_speed(0))
    time.sleep(0.5)

    d.key(K_A)       # AA off: the colour count must measure shading
    time.sleep(0.4)
    flat_2d = d.canvas_image("scene-2d")
    colors_2d = d.distinct_colors(flat_2d)

    # Shading belongs to the solids: refused in the 2D scene, and logged
    # so this sees the refusal rather than inferring it from silence.
    got = d.key_until(K_F, "gfxdemo: shaded ignored")
    d.check_log("F in the 2D scene is refused, not applied", got, "gfxdemo: shaded ignored")
    d.check("...and nothing was toggled", not any("shaded on" in l for l in got),
            f"{[l for l in got if 'shaded' in l]}")

    got = d.key_until(K_S, "gfxdemo: scene cube")
    d.check_log("pressing S moves on to the cube", got, "gfxdemo: scene cube")
    time.sleep(0.5)
    cube = d.canvas_image("scene-3d")
    d.check("the cube scene draws something different",
            d.differing_fraction(flat_2d, cube) > 0.01,
            "the canvas barely changed when the scene switched")

    colors_3d = d.distinct_colors(cube)
    print(f"        ({colors_2d} distinct colours in 2D aliased, {colors_3d} in 3D)")
    d.check("the cube's edges are shaded by depth",
            colors_3d > colors_2d,
            f"{colors_3d} distinct colours in the 3D scene vs {colors_2d} in the "
            "2D one, both aliased -- a per-edge depth shade is the only thing "
            "that can add them, and it needs a real projection to exist")

    # A solid inks several times what a wireframe does; its lit faces are
    # flat colours covering a face's worth of pixels each; and switching
    # back restores the wireframe exactly, since nothing else moved.
    got = d.key_until(K_F, "gfxdemo: shaded on")
    d.check_log("pressing F fills the cube", got, "gfxdemo: shaded on")
    time.sleep(0.5)
    solid = d.canvas_image("scene-3d-shaded")
    wire_ink, solid_ink = d.ink(cube), d.ink(solid)
    print(f"        ({wire_ink} inked pixels as a wireframe, {solid_ink} shaded)")
    d.check("a shaded cube is a solid, not an outline",
            solid_ink > 3 * wire_ink,
            f"{solid_ink} inked pixels shaded vs {wire_ink} as a wireframe")
    d.check("a wireframe has no face-sized flat colour",
            len(d.big_colors(cube)) == 0,
            f"{d.big_colors(cube)} -- the control: an outline must not trip the face check")
    got = d.key_until(K_F, "gfxdemo: shaded off")
    d.check_log("pressing F again restores the wireframe", got, "gfxdemo: shaded off")
    time.sleep(0.5)
    wire_again = d.canvas_image("scene-3d-wire-again")
    d.check("the wireframe comes back pixel-identical",
            d.differing_fraction(cube, wire_again) == 0.0,
            "the fill left something behind, or the angle moved at speed 0")

    # Round trip taken NOW: nothing between the two captures may move
    # the angle. By the toolbar, so both paths into a scene are covered.
    got = d.click_button_until(B_2D, "gfxdemo: scene 2d")
    d.check_log("the 2D toolbar button switches back", got, "gfxdemo: scene 2d")
    time.sleep(0.5)
    back = d.canvas_image("scene-2d-again")
    d.check("returning to the 2D scene restores it exactly",
            d.differing_fraction(flat_2d, back) == 0.0,
            "the 2D scene came back different from how it was left -- at "
            "speed 0 with the angle unchanged it must be pixel-identical")

    # The animation pair, which needs the angle to move.
    d.key(K_2)
    time.sleep(0.3)
    d.check("speed returns for the cube", d.set_speed(4))
    a = d.canvas_image("cube-spin-a")
    time.sleep(0.7)
    b = d.canvas_image("cube-spin-b")
    d.check("the cube is rotating", d.differing_fraction(a, b) > 0.01,
            "the cube did not move between frames")

    # LIT, not merely filled: one base colour, so only a light can make
    # the big faces differ from one moment to the next as it turns.
    got = d.key_until(K_F, "gfxdemo: shaded on")
    d.check_log("F fills the spinning cube", got, "gfxdemo: shaded on")
    time.sleep(0.4)
    lit_a = set(d.big_colors(d.canvas_image("cube-lit-a")))
    time.sleep(0.7)
    lit_b = set(d.big_colors(d.canvas_image("cube-lit-b")))
    print(f"        (face colours {sorted(lit_a)} then {sorted(lit_b)})")
    d.check("a face's colour changes as it turns through the light",
            lit_a and lit_b and lit_a != lit_b,
            f"{sorted(lit_a)} then {sorted(lit_b)} -- one base colour and a "
            "fixed light must shade a turning face differently over time")
    got = d.key_until(K_F, "gfxdemo: shaded off")
    d.check_log("F restores the wireframe", got, "gfxdemo: shaded off")

    d.check("the cube stops at speed 0", d.set_speed(0))
    time.sleep(0.5)
    s1 = d.canvas_image("cube-still-a")
    time.sleep(0.7)
    s2 = d.canvas_image("cube-still-b")
    still = d.differing_fraction(s1, s2)
    d.check("at speed 0 the cube is static", still < 0.001,
            f"{still:.4%} of sampled pixels still changed at speed 0")

    # Hand the rest of the run the state it expects: 2D, AA on, speed 3.
    d.key(K_1)
    d.set_speed(3)
    d.key(K_A)


def greyish(img):
    """Pixels whose channels are close together and not dark -- the
    checker's light squares, shaded. The untextured teapot is strongly
    blue (90, 200, 250 times its light) and has none."""
    n = 0
    for count, (r, g, b) in img.getcolors(1 << 24) or []:
        if r > 60 and abs(r - b) < 24 and abs(g - b) < 24:
            n += count
    return n


def check_teapot(d):
    """The teapot: a solid, a texture, a spin. What its triangles and
    depth buffer do per pixel is teapot_hostcheck.py's; this checks the
    app draws it and the toggles reach it."""
    got = d.click_button_until(B_TEAPOT, "gfxdemo: scene teapot")
    d.check_log("the Teapot toolbar button shows the teapot", got, "gfxdemo: scene teapot")
    d.check("speed reaches 0 for the teapot", d.set_speed(0))
    time.sleep(0.5)
    wire = d.canvas_image("teapot-wire")

    got = d.key_until(K_F, "gfxdemo: shaded on")
    d.check_log("F fills the teapot", got, "gfxdemo: shaded on")
    time.sleep(0.6)
    solid = d.canvas_image("teapot-solid")
    wire_ink, solid_ink = d.ink(wire), d.ink(solid)
    print(f"        ({wire_ink} inked pixels as a wireframe, {solid_ink} shaded)")
    d.check("the shaded teapot is drawn, and fills more than its wireframe",
            solid_ink > 5000 and solid_ink > wire_ink,
            f"{solid_ink} inked shaded vs {wire_ink} as a wireframe")
    grey_plain = greyish(solid)

    got = d.key_until(K_T, "gfxdemo: textured on")
    d.check_log("T textures the teapot", got, "gfxdemo: textured on")
    time.sleep(0.6)
    tex = d.canvas_image("teapot-textured")
    grey_tex = greyish(tex)
    print(f"        ({grey_plain} checker-light pixels untextured, {grey_tex} textured)")
    d.check("the checker reaches the teapot's pixels",
            grey_tex > 2000 and grey_plain < grey_tex // 10,
            f"{grey_tex} checker-light pixels textured vs {grey_plain} plain")

    d.check("the teapot turns", d.set_speed(4))
    a = d.canvas_image("teapot-spin-a")
    time.sleep(0.8)
    b = d.canvas_image("teapot-spin-b")
    d.check("the textured teapot is rotating", d.differing_fraction(a, b) > 0.01,
            "the teapot did not move between frames")
    d.check("the teapot stops at speed 0", d.set_speed(0))
    time.sleep(0.6)
    s1 = d.canvas_image("teapot-still-a")
    time.sleep(0.8)
    s2 = d.canvas_image("teapot-still-b")
    still = d.differing_fraction(s1, s2)
    d.check("at speed 0 the teapot is static", still < 0.001,
            f"{still:.4%} of sampled pixels still changed at speed 0")


def check_resize(d):
    """Narrow: the toggles fold into View, the menu commits, Esc shuts
    it. Minimum: the scenes and speed controls stay. Wide again: nothing
    folded. And every size, the scene fits its canvas."""
    home = d.size
    full = d.lay.get("toolbar.shown")
    d.check("at its opening size the whole toolbar shows", "toolbar.more" not in d.lay,
            f"shown {full}, more {d.lay.get('toolbar.more')}")
    wide_canvas = d.canvas

    w, h = d.resize(440, 520)
    d.check("the window takes a narrower size", w == 440, f"content {w}x{h}")
    ok = d.wait_layout(lambda l: "toolbar.more" in l)
    shown = d.lay.get("toolbar.shown", 99)
    d.check("narrow, the toolbar folds its tail into a View button",
            ok and KEEP_SHOWN <= shown < 12, f"shown {shown}, layout {d.lay}")
    c = d.canvas
    d.check("the canvas takes the new width and height",
            c and c[2] == w - 20 and c[3] > wide_canvas[3],
            f"canvas {c} in a {w}x{h} window, was {wide_canvas}")
    time.sleep(0.5)
    img = d.canvas_image("narrow")
    e = d.edge_ink(img)
    d.check("the scene scales to the narrow canvas", e == 0,
            f"{e} inked pixels on the canvas's outermost 3 pixels")

    # The View menu: its rows are the folded buttons, ticked and
    # committed through the same item_flags and codes.
    d.click_rect_until(d.lay["toolbar.more"], "")
    ok = d.wait_layout(lambda l: l.get("toolbar.open") == 1 and ("row", 0) in l)
    rows = sorted(k[1] for k in d.lay if isinstance(k, tuple) and k[0] == "row")
    d.check("the View button opens a menu of the folded items",
            ok and len(rows) == 12 - shown - (1 if shown <= 7 else 0),
            f"rows {rows} with {shown} shown")
    # Textured is the menu's LAST row whenever it has folded.
    got = d.click_rect_until(d.lay[("row", rows[-1])], "gfxdemo: textured")
    d.check_log("the menu's last row toggles the texture", got, "gfxdemo: textured off")
    d.check("...and closes the menu",
            d.wait_layout(lambda l: l.get("toolbar.open") == 0), f"{d.lay}")
    d.click_rect_until(d.lay["toolbar.more"], "")
    d.wait_layout(lambda l: l.get("toolbar.open") == 1)
    d.key(K_ESC)
    d.check("Esc shuts the View menu",
            d.wait_layout(lambda l: l.get("toolbar.open") == 0), f"{d.lay}")

    # The minimum: asked for far less, the window stops where the scenes
    # and the speed controls still fit.
    w, h = d.resize(200, 150)
    d.check("the window refuses to go below its minimum", w > 200 and h > 150,
            f"content {w}x{h}")
    shown = d.lay.get("toolbar.shown", 0)
    d.check("at the minimum the scenes and speed controls still show",
            shown >= KEEP_SHOWN, f"shown {shown}")

    w, h = d.resize(*home)
    d.check("wide again, nothing is folded",
            d.wait_layout(lambda l: "toolbar.more" not in l and l.get("toolbar.shown") == 12),
            f"{d.lay}")
    time.sleep(0.5)
    e = d.edge_ink(d.canvas_image("wide-again"))
    d.check("the scene scales back to the wide canvas", e == 0,
            f"{e} inked pixels on the canvas's outermost 3 pixels")


def run(d):
    print("gfxdemo_test: checks")

    # 1. It came up, and it is a ring-3 client.
    d.check_log("app announced itself", d.startup, "gfxdemo: ready")
    d.check_log("anti-aliasing starts on", d.startup, "gfxdemo: aa on")
    pid = d.win.get("client_pid", 0)
    d.check("window is a ring-3 client, not kernel-space", pid > 0,
            f"client_pid was {pid} -- 0 means the WM drew it in ring 0")
    d.check("the window is resizable", d.win.get("resizable") is True, f"{d.win}")

    d.park_cursor()

    # 2. It animates -- and stops when told to.
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

    # 3. The AA toggle reaches the rasteriser.
    aa_on = d.distinct_colors(s2)
    got = d.key_until(K_A, "gfxdemo: aa off")
    d.check_log("pressing A turns anti-aliasing off", got, "gfxdemo: aa off")
    time.sleep(0.4)
    aliased = d.canvas_image("aliased")
    aa_off = d.distinct_colors(aliased)
    print(f"        ({aa_on} distinct colours with AA, {aa_off} without)")
    d.check("anti-aliasing actually changes the rasteriser",
            aa_on > aa_off * 2,
            f"{aa_on} distinct colours with AA vs {aa_off} without -- "
            "a real AA path emits partial coverage, so many more")
    got = d.click_button_until(B_AA, "gfxdemo: aa on")
    d.check_log("the Smooth edges button turns it back on", got, "gfxdemo: aa on")

    d.check("speed returns from the keyboard", d.set_speed(3))

    check_cube(d)
    check_teapot(d)

    # The speed buttons, at the centre the APP reported.
    got = d.click_button_until(B_FASTER, "gfxdemo: speed 1")
    d.check_log("the + button changes speed", got, "gfxdemo: speed 1")

    try:
        check_resize(d)
    finally:
        if d.size != d.home:
            d.resize(*d.home)   # the size is remembered -- see the docstring

    # It exits cleanly when asked, rather than being killed.
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
                    help="also write shapes-demo.png here")
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
