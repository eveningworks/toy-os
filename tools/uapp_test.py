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
from gui_debug import DebugConsole, enter_gui
from qmp_test import QMPSession

DEFAULT_SOCK = ".vm.serial"
TITLE = "Ring 3 Client"
SPAWN_PATH = "/tests/winclient"   # spawned directly -- see run()
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
    # `gui spawn`, not a Terminal typing `run winclient`: the kernel-space
    # Terminal retired in M41's stage 0, and the ring-3 one has no window
    # yet when the injected keys would arrive.
    dbg.send(f"gui spawn {SPAWN_PATH}")

    # POLLED UNTIL THE HINT IS THERE, not merely until the window is.
    # The next check reads `resizable`, which arrives over TWP after the
    # window itself does -- so a poll that exits on "the window exists"
    # is weaker than what follows it, and under the full suite's load
    # (eight guests on one host) it read False on a window that was
    # perfectly resizable a moment later. The assertion is unchanged: if
    # the hint never comes, this still times out and still fails.
    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win and win.get("resizable"):
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

    check_maximize(dbg, qmp, tmp, res)


# The old WIN_CLIENT_MAX_W/H. Named rather than inlined because what is
# being asserted is "past the size that used to be refused", and the
# constant it refers to no longer exists in the tree.
OLD_CAP_W, OLD_CAP_H = 640, 480


def check_maximize(dbg, qmp, tmp, res):
    """Maximizing a CLIENT window, which is a resize the client must ack.

    Two things are under test and they are worth separating.

    **Maximize is a proposal, not an imposition.** The WM owns where a
    window sits and how big its frame is; it does NOT own a client's
    pixel buffer. Setting `w->w`/`w->h` directly -- which both maximize
    call sites did until this check was written -- gives full-screen
    chrome around a buffer still at the old size, with undrawn desktop
    filling the difference. That is the exact failure
    abi/win_proto.h's configure/ack section describes for the grip, and
    it went unnoticed because nothing maximized a client window.

    **The buffer that results is past the old 640x480 cap**, and past
    what the old 2 MiB WIN_BUFFER_STRIDE could address at all -- a
    full-screen buffer is ~3.2 MiB. Maximize is used to get there rather
    than a grip drag because the drag's reachable size depends on where
    the window happens to sit, which made the same assertion pass or
    fail on window POSITION.

    Asserted as a PAIR, the same way checks 3/4 are: the reported
    content size and the client's PAINTED extent must both reach the new
    size. A server that accepted the request and handed back a buffer
    still bounded by the stride would satisfy either half alone.

    Positive control, done by hand when this was written: restore the
    direct `windows[idx].w = screen_w` assignment in
    wm_input.c's wm_toggle_maximize(). The reported size still grows --
    check 7 stays green -- and the painted-extent half goes red, which
    is the half that was missing.
    """
    dbg.state()  # asserts the WM is answering before anything below

    win = dbg.window(TITLE)
    if not win:
        res.check("the client window survived to be maximized", False, "no window")
        return

    # The maximize button is the middle of the three at the title bar's
    # right end. Its centre comes from the WM rather than from pixel
    # arithmetic here -- see gui_debug.py.
    dbg.click(win["x"] + win["w"] - 40, win["y"] + 12)
    dbg.settle()
    time.sleep(0.8)

    big = dbg.window(TITLE)
    bc = big["content"] if big else None
    res.check("maximizing a client window grows it past the old 640x480 cap",
              bc is not None and bc["w"] > OLD_CAP_W and bc["h"] > OLD_CAP_H,
              f"content {bc['w']}x{bc['h']}" if bc else "window gone")

    if bc:
        # Three points: the far corner, a point beyond the old cap on
        # BOTH axes, and one that must NOT be the border -- the
        # neighbour half of the assertion docs/gui-guidelines.md asks
        # for. The interior points are clamped inside the content so a
        # smaller screen shrinks the claim rather than sampling chrome.
        ix = min(bc["x"] + OLD_CAP_W + 8, bc["x"] + bc["w"] - 6)
        iy = min(bc["y"] + OLD_CAP_H + 8, bc["y"] + bc["h"] - 6)
        corner = pixel(qmp, tmp, "uapp_max.png",
                       bc["x"] + bc["w"] - 2, bc["y"] + bc["h"] - 2)
        beyond = pixel(qmp, tmp, "uapp_max.png", ix, iy)
        painted_past_cap = ix > bc["x"] + OLD_CAP_W and iy > bc["y"] + OLD_CAP_H
        res.check("the client painted the whole maximized buffer",
                  corner == BORDER and beyond != BORDER and painted_past_cap,
                  f"corner {corner} (want {BORDER}), interior at "
                  f"({ix - bc['x']},{iy - bc['y']}) {beyond} (want != {BORDER}), "
                  f"past cap on both axes: {painted_past_cap}")

    # Restore, and require it to come back -- the reverse direction is a
    # resize too, and a client that only handled growth would pass
    # everything above and leave a full-screen buffer behind.
    dbg.click(big["x"] + big["w"] - 40, big["y"] + 12)
    dbg.settle()
    time.sleep(0.8)
    back = dbg.window(TITLE)
    res.check("restoring a maximized client window shrinks its buffer back",
              back is not None and back["content"]["w"] < OLD_CAP_W,
              f"content {back['content']['w']}x{back['content']['h']}"
              if back else "window gone")


def check_worker_post(dbg, qmp, tmp, res):  # noqa: ARG001 -- tmp unused
    """A worker thread wakes the main loop, and the callback runs on MAIN.

    uapp_post() is the one Toykit call a worker thread may make, and the
    property that matters is not "on_user ran" -- it is that some OTHER
    thread posted it and the MAIN thread ran it. Only the pair of tids
    says that, which is why winclient logs both.

    A build where uapp_post() silently did nothing, or where the
    compositor refused a self-post, leaves no line at all; a build where
    the callback somehow ran on the worker would leave one with two
    equal tids. Both are distinguishable failures rather than a timeout.
    """
    win = dbg.window(TITLE)
    if not win:
        res.check("6. a worker thread woke the main loop", False, "no window")
        return

    dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    dbg.settle()
    before = len(dbg.logs("winclient: on_user"))
    # 'p' for post -- NOT 'w', which keyup_test.py drives (see winclient.c).
    qmp.send_key("p")

    # The worker sleeps 50 ms before posting, so this waits on the LINE
    # rather than on a fixed delay -- see CLAUDE.md on polls whose exit
    # condition is weaker than what follows them.
    deadline = time.time() + 5
    lines = []
    while time.time() < deadline:
        lines = dbg.logs("winclient: on_user")
        if len(lines) > before:
            break
        time.sleep(0.2)

    res.check("6. a worker thread woke the main loop",
              len(lines) > before,
              "no on_user line -- uapp_post() reached nothing")
    if len(lines) <= before:
        return

    fields = {}
    for part in lines[-1].split():
        if "=" in part:
            k, _, v = part.partition("=")
            try:
                fields[k] = int(v)
            except ValueError:
                pass

    res.check("6b. the payload arrived unchanged", fields.get("a0") == 42,
              f"a0={fields.get('a0')}")
    # THE LOAD-BEARING ONE. Everything else here would pass if uapp_post()
    # were called on the main thread and delivered synchronously.
    res.check("6c. the poster was NOT the thread that ran the callback",
              fields.get("from_tid") not in (None, fields.get("on_tid")),
              f"from_tid={fields.get('from_tid')} on_tid={fields.get('on_tid')}")


def check_focus_caret(dbg, qmp, tmp, res):
    """A caret must not be drawn while its window is unfocused.

    An unfocused window showing a caret claims to be taking input that
    is actually going somewhere else -- which is the whole reason TWP
    has WIN_EV_FOCUS. Terminal draws one; winclient does not, so it
    makes a convenient thief of focus.

    Asserted as a ROUND TRIP rather than by hunting for the caret's
    pixels: capture Terminal's content focused, take focus away, and
    require the region to CHANGE; give focus back and require it to
    match the first capture EXACTLY. "It changed" alone would be
    satisfied by almost anything; "it came back identical" is what says
    the only difference was the caret.

    The two windows are moved apart first -- an occluded region would
    differ for reasons that have nothing to do with focus.
    """
    from PIL import Image

    def region(name, box):
        p = os.path.abspath(os.path.join(tmp, name))
        qmp.screenshot(p)
        with Image.open(p) as im:
            return im.convert("RGB").crop(box).tobytes()

    # winclient is quit first ('q', see winclient.c) so the focus checks
    # below have a predictable pair of windows on screen.
    win = dbg.window(TITLE)
    if win:
        dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
        dbg.send("gui key q")
        dbg.settle()
        deadline = time.time() + 5
        while time.time() < deadline and dbg.window(TITLE):
            time.sleep(0.2)

    # The ring-3 Terminal, spawned directly (see run()'s note).
    dbg.send("gui spawn /bin/wm/apps/uterm")

    deadline = time.time() + SPAWN_TIMEOUT_S
    term = None
    while time.time() < deadline:
        term = dbg.window("Terminal")
        if term:
            break
        time.sleep(0.3)
    if not term:
        res.check("a ring-3 Terminal opened, to test the caret", False,
                  "no 'Terminal' window")
        return

    dbg.drag(term["x"] + 60, term["y"] + 8, 700 + 60, term["y"] + 8)
    dbg.settle()
    term = dbg.window("Terminal")
    c = term["content"]
    box = (c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"])

    # Focus it EXPLICITLY rather than assuming the spawn left it focused
    # -- clicked in the CONTENT, not the title bar, since the window that
    # steals focus below opens near the left edge and a title-bar click
    # can land on whatever is stacked there. Assuming the baseline state
    # is what made this check compare two unfocused captures and report
    # "pixel-identical" when the caret was working correctly.
    dbg.click(c["x"] + c["w"] - 30, c["y"] + c["h"] - 30)
    dbg.settle()
    time.sleep(0.4)
    focused = region("focus_a.png", box)

    # A second window steals focus. The kernel-space Terminal used to
    # play this part; it retired in M41's stage 0, so a kernel app that
    # is still kernel-side takes it -- the Terminal under test was
    # dragged clear to the right above, so this cannot overlap it.
    dbg.open_app("System Settings")
    dbg.settle()
    thief = dbg.window("System Settings")
    if thief is None:
        res.check("a second window exists to take focus", False, "no 'System Settings' window")
        return
    dbg.click(thief["x"] + 40, thief["y"] + 8)
    dbg.settle()
    time.sleep(0.4)
    unfocused = region("focus_b.png", box)
    res.check("an unfocused window stops drawing its caret",
              unfocused != focused,
              "the Terminal's content is pixel-identical focused and unfocused")

    # And back -- same point as the first focusing click, so the caret is
    # in the same place and "restores it exactly" can be exact.
    dbg.click(c["x"] + c["w"] - 30, c["y"] + c["h"] - 30)
    dbg.settle()
    time.sleep(0.4)
    refocused = region("focus_c.png", box)
    res.check("refocusing restores it exactly",
              refocused == focused,
              "content differs from the original focused capture")


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
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, args.tmp, res)
        # BEFORE check_focus_caret, which quits winclient with 'q'.
        check_worker_post(dbg, qmp, args.tmp, res)
        check_focus_caret(dbg, qmp, args.tmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nuapp_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
