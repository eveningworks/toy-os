#!/usr/bin/env python3
"""tools/uapp_test.py -- the TWP resize handshake, end to end.

WHAT THIS COVERS
----------------
Resize is a configure/ack handshake (see kernel/include/abi/win_proto.h):
the window manager PROPOSES a size with WIN_EV_RESIZE and draws an
outline, the client answers with WIN_REQ_RESIZE, and only then does the
window actually change. Nothing else in the suite exercises it.

It drives `winclient` (userland/tests/winclient.c), which is the right
target precisely because it contains no resize code at all -- it sets
`.flags = UAPP_RESIZABLE` and nothing else. Everything being tested here
is therefore Toykit's and TWS's, which is the claim worth checking: an
app that has never heard of resizing resizes correctly.

THE ASSERTIONS THAT MATTER
--------------------------
Two of these are paired on purpose, because either half alone passes for
the wrong reasons:

  * The window's reported size AND the client's painted extent must
    BOTH change. A client that resized its buffer but not its drawing
    passes any check that looks at only one of them -- and the failure
    it hides is exactly the one the handshake exists to prevent (chrome
    growing around pixels that are still the old size).
  * A resize below the client's declared minimum must CLAMP rather than
    be refused or obeyed, and the window must still be usable after.

winclient paints a 3px border around its whole surface, which is what
makes "the painted extent" measurable: the border pixel is at the new
corner if and only if the client redrew at the new size.

POSITIVE CONTROL
----------------
A clean run of this proves nothing on its own. To check it can fail,
break the ack: in userland/ui/uapp.c's WIN_EV_RESIZE arm, return without
calling uapp_resize(). The window then keeps its old size and checks 3
and 4 go red. Done once by hand when this was written; do it again
before trusting a green run after any change to the handshake.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole
from qmp_test import QMPSession

DEFAULT_SOCK = ".vm.serial"
TITLE = "Ring 3 Client"
SPAWN_CMD = "run winclient"
SPAWN_TIMEOUT_S = 15.0

# winclient's border colour (0xECF0F1) and one of its fills (0x2E4053).
BORDER = (236, 240, 241)


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def pixel(qmp, tmp, name, x, y):
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    with Image.open(p) as im:
        return im.convert("RGB").getpixel((x, y))


def run(dbg, qmp, tmp, res):
    dbg.send("gui open Terminal")
    dbg.settle()
    for ch in SPAWN_CMD:
        dbg.send(f"gui key {'0x20' if ch == ' ' else ch}")
    dbg.settle()
    dbg.send("gui key 0x0d")

    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win:
            break
        time.sleep(0.3)
    res.check("winclient runs as a ring-3 client with its own window", win is not None,
              f"no window titled {TITLE!r} within {SPAWN_TIMEOUT_S}s")
    if not win:
        return

    # 2. The hint reached TWS. Before TWP carried hints this was FALSE
    #    for every client window that has ever existed, because the WM
    #    read resizability off a struct only kernel-space apps have.
    res.check("a client window can declare itself resizable",
              win["resizable"] is True,
              f"gui windows reports resizable={win['resizable']}")

    before_w, before_h = win["w"], win["h"]
    c = win["content"]

    # 3/4. Drag the bottom-right grip out by a known amount.
    grow_x, grow_y = 92, 72
    dbg.drag(win["x"] + win["w"] - 2, win["y"] + win["h"] - 2,
             win["x"] + win["w"] - 2 + grow_x, win["y"] + win["h"] - 2 + grow_y)
    dbg.settle()
    time.sleep(0.6)

    after = dbg.window(TITLE)
    grew = after and (after["w"], after["h"]) == (before_w + grow_x, before_h + grow_y)
    res.check("dragging the grip resizes the window by the dragged amount", grew,
              f"{before_w}x{before_h} -> {after['w']}x{after['h']} if after else 'gone'")
    if not after:
        return

    # The paired half: the CLIENT repainted at the new size. Its border
    # sits at the new bottom-right corner if and only if it did.
    ac = after["content"]
    corner = pixel(qmp, tmp, "uapp_resized.png", ac["x"] + ac["w"] - 2, ac["y"] + ac["h"] - 2)
    old_corner = pixel(qmp, tmp, "uapp_resized.png",
                       ac["x"] + c["w"] - 2, ac["y"] + c["h"] - 2)
    res.check("the client repainted at the new size, not just the chrome",
              corner == BORDER and old_corner != BORDER,
              f"new corner {corner} (want {BORDER}), old corner {old_corner} (want != {BORDER})")

    # 5. Below the declared minimum, the proposal is clamped rather than
    #    obeyed. winclient asks for min 120x80 content.
    dbg.drag(after["x"] + after["w"] - 2, after["y"] + after["h"] - 2,
             after["x"] + 10, after["y"] + 10)
    dbg.settle()
    time.sleep(0.6)
    small = dbg.window(TITLE)
    res.check("a resize below the client's declared minimum is clamped",
              small is not None and small["content"]["w"] >= 120 and small["content"]["h"] >= 80,
              f"content {small['content']['w']}x{small['content']['h']} if small else 'gone'")

    # 6. Still alive and still drawing after two resizes -- a
    #    reallocation that leaked or unmapped the wrong frames would
    #    show up here as a dead or blank window.
    if small:
        sc = small["content"]
        edge = pixel(qmp, tmp, "uapp_small.png", sc["x"] + 1, sc["y"] + 1)
        res.check("the client is still drawing after being resized twice",
                  edge == BORDER, f"top-left content pixel {edge}, want {BORDER}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(3)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, args.tmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nuapp_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
