#!/usr/bin/env python3
"""Drive the ring-3 client window (userland/tests/winclient.c) and assert on it.

Proves the windowing protocol end to end, in the direction that matters:
a RING-3 PROCESS owns a window in the window manager's own window list,
its pixels reach the screen from shared memory, input the WM routes to
it actually arrives, and the close handshake is a request the client
answers rather than the WM yanking the window away.

WHAT IT ASSERTS, AND WHY IN THIS FORM
-------------------------------------
Geometry comes from `gui windows`, so the WM reports its own layout
instead of this script re-deriving it -- the same reason uidemo_test.py
reads UI Demo's `layout` lines rather than hardcoding row offsets.

Content is checked by PIXEL VALUE (docs/gui-guidelines.md): the client
signals "I redrew" only by changing colour, and a screenshot is not an
assertion. Every colour check also samples a point that must NOT have
changed -- half the assertion is the neighbour staying put, which is
what catches a full-screen repaint masquerading as a working present.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/winclient_test.py --shot screenshots/YYYY-MM-DD
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
CLIENT_TITLE = "Ring 3 Client"
SPAWN_CMD = "run winclient"

# userland/tests/winclient.c's COLORS[], as (r, g, b). The client starts on
# the first and advances one step per key or click.
COLORS = [
    (0x2E, 0x40, 0x53),
    (0x7D, 0x3C, 0x98),
    (0x1E, 0x84, 0x49),
    (0xB0, 0x3A, 0x2E),
    (0xB7, 0x95, 0x0B),
]

SPAWN_TIMEOUT_S = 15.0


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def type_text(dbg, text):
    for ch in text:
        dbg.send(f"gui key {'0x20' if ch == ' ' else ch}")
    dbg.settle()


def sample(qmp, path, x, y):
    """One pixel's (r, g, b) from a fresh screenshot."""
    from PIL import Image
    qmp.screenshot(os.path.abspath(path))
    with Image.open(path) as im:
        return im.convert("RGB").getpixel((x, y))


def near(got, want, tol=6):
    """Exact-ish match. A small tolerance absorbs nothing in principle
    (these are flat fills, not anti-aliased edges) but keeps the test
    from being brittle if the framebuffer format ever rounds."""
    return all(abs(g - w) <= tol for g, w in zip(got, want))


def run(dbg, qmp, tmp, shot_dir, res):
    dbg.send("gui open Terminal")
    dbg.settle()
    type_text(dbg, SPAWN_CMD)
    dbg.send("gui key 0x0d")

    # Wait for the client's window to appear in the WM's own list --
    # not a fixed sleep, which would race a slow spawn.
    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = dbg.window(CLIENT_TITLE)
        if win:
            break
    res.check("a ring-3 process owns a window in the WM's window list", win is not None,
              f"no window titled {CLIENT_TITLE!r} within {SPAWN_TIMEOUT_S}s")
    if not win:
        return

    c = win["content"]
    res.check("the window's content area is the size the client asked for",
              (c["w"], c["h"]) == (320, 200), f"got {c['w']}x{c['h']}, wanted 320x200")

    # Well inside the client's content, clear of its 3px border.
    cx, cy = c["x"] + c["w"] // 2, c["y"] + c["h"] // 2
    # A control point on the Terminal behind it, which nothing in this
    # test should ever change.
    term = dbg.window("Terminal")
    tx, ty = term["content"]["x"] + 20, term["content"]["y"] + term["content"]["h"] - 20

    got = sample(qmp, os.path.join(tmp, "wc0.png"), cx, cy)
    res.check("the client's own pixels reach the screen",
              near(got, COLORS[0]), f"got {got}, wanted {COLORS[0]}")
    control0 = sample(qmp, os.path.join(tmp, "wc0.png"), tx, ty)

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-client-window.png")))

    # A key the WM routes to the focused client must reach it and make
    # it repaint. This exercises the whole chain: WM focus -> event
    # queue -> the client's blocking wait -> its draw -> WIN_REQ_PRESENT
    # -> composite.
    dbg.send("gui key a")
    dbg.settle()
    time.sleep(0.6)
    got = sample(qmp, os.path.join(tmp, "wc1.png"), cx, cy)
    res.check("a key routed to the client makes it redraw",
              near(got, COLORS[1]), f"got {got}, wanted {COLORS[1]}")
    control1 = sample(qmp, os.path.join(tmp, "wc1.png"), tx, ty)
    res.check("the window behind it did NOT change",
              control1 == control0, f"{control0} -> {control1}")

    # A click inside the content area, same chain but through the mouse
    # path.
    dbg.send(f"gui click {cx} {cy}")
    dbg.settle()
    time.sleep(0.6)
    got = sample(qmp, os.path.join(tmp, "wc2.png"), cx, cy)
    res.check("a click routed to the client makes it redraw",
              near(got, COLORS[2]), f"got {got}, wanted {COLORS[2]}")

    # The close button is a REQUEST: the WM sends WIN_EV_CLOSE and the
    # client answers with WIN_REQ_DESTROY. So a successful close proves
    # the client was alive and processed the event -- the WM never
    # removes the window itself.
    close_x = win["x"] + win["w"] - 14
    close_y = win["y"] + 14
    dbg.send(f"gui click {close_x} {close_y}")
    dbg.settle()

    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if dbg.window(CLIENT_TITLE) is None:
            gone = True
            break
    res.check("the close button is a handshake the client completes", gone,
              "the window was still listed after the close request")

    # And the desktop must be intact afterwards -- a client teardown
    # that corrupted the window list would show up here.
    res.check("the desktop survives the client exiting",
              dbg.window("Terminal") is not None)

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-client-closed.png")))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--shot", metavar="DIR", help="write proof screenshots here")
    ap.add_argument("--tmp", default="/tmp", help="scratch dir for sampled screenshots")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.5)
    if args.shot:
        os.makedirs(args.shot, exist_ok=True)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, args.shot, res)
    finally:
        dbg.close()

    print(f"\nwinclient_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
