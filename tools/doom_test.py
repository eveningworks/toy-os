#!/usr/bin/env python3
"""tools/doom_test.py -- DOOM runs, draws, animates and takes input.

ON DEMAND, NOT IN THE GATE. It needs an IWAD, and an IWAD is not in this
repository (`tools/fetch_wad.py`, and see `userland/ports/doom/README.md`
for why). A checkout without one gets a clean SKIP rather than a
failure, because "the user has not fetched a 4 MB game asset" is not a
regression.

WHAT IT IS FOR
--------------
Doom is this tree's largest ring-3 program by an order of magnitude --
~36,000 lines of somebody else's C, exercising the allocator, stdio over
a real 4 MB file, the ELF loader, floating point, the window protocol
and both edges of the keyboard at once. Most of what it covers is
covered elsewhere in smaller pieces; what is NOT covered elsewhere is
all of it running together for minutes.

THE ASSERTIONS THAT MATTER, and why each is shaped the way it is:

  * **It ANIMATES.** Sampled repeatedly rather than twice, because
    Doom's attract mode cycles demo -> title -> credits and the title
    screen is legitimately STATIC. A two-sample check lands on the title
    screen often enough to be a flake, and the first version of this did
    exactly that. Any consecutive pair differing is the claim.
  * **A key CHANGES the screen.** Escape opens the menu, which is a
    large, unmistakable pixel change. This is the check that would have
    been impossible before WIN_EV_KEY_UP existed -- not because Escape
    needs a release, but because nothing could have got this far: Doom's
    DG_GetKey() asks for an EDGE, and a press-only OS cannot answer it.
  * **Enter STARTS A NEW GAME, asserted by the screen going STILL.**
    Added after the port shipped unable to start one (0x0A against
    KEY_ENTER's 0x0D). The first version of this check pressed Enter and
    asserted the screen CHANGED -- and passed with the bug reverted,
    because the attract demo animates behind the menu regardless. A
    started game is the one state in which nothing moves, since the
    player is standing still.
  * **Ctrl FIRES.** Doom's `key_fire` is the abstract KEY_FIRE (0xa3),
    not a Ctrl keycode, and the platform layer is what maps onto it --
    this port first mapped Ctrl to KEY_RCTRL, which is bound to nothing,
    so firing did nothing at all while every menu key worked. Same
    static-screen oracle, inverted.
  * **It maximizes, and the IMAGE stays 4:3.** Measured from the ink
    rather than from the window: the bars are exactly black and Doom's
    own darkest pixels are not, so scanning in from the edges for a
    non-black column gives the drawn width directly. A check that only
    asserted the window resized would pass on a build that stretched the
    picture into the wrong shape, which is the actual failure mode.
  * **The frame RATE.** Doom targets 35Hz and sleeps to hold it, so a
    number well under that means the emulator is not keeping up. Asserted
    loosely (>15 fps) because this is TCG and the host is shared -- the
    point is to catch a collapse to single digits, not to benchmark.
    Measured ~34 fps on the machine this was written on.

POSITIVE CONTROL
----------------
Break the blit: in `userland/gui/apps/doom.c`'s `on_draw`, return before
`ugfx_blit()`. MEASURED: 3 pass, 3 fail. "draws real content",
"animates" AND "a keystroke reaches the game" all go red -- that last
one because it is a PIXEL check like the other two, so it can only mean
anything when drawing works. Worth knowing: it is not an independent
witness to input, and a run where all three fail together says
"nothing is being painted", not "input is broken".

The frame-rate check stays GREEN and the number goes UP (59.5 fps
against 34.3), which is the useful part: the rate is measured from the
game's own finished frames, so it keeps reporting honestly while the
window shows nothing at all -- and it goes faster precisely because it
is no longer paying for a blit.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui
from qmp_test import QMPSession

try:
    from PIL import Image, ImageChops
except ImportError:
    print("doom_test: needs Pillow (pip install pillow)", file=sys.stderr)
    sys.exit(2)

DEFAULT_SOCK = ".vm.serial"
EXEC = "/bin/wm/apps/doom"
# The title Doom sets for ITSELF once it has loaded a WAD -- so matching
# on it is also the check that DG_SetWindowTitle round-tripped. The app
# opens as plain "DOOM".
TITLE_LOADED = "DOOM Shareware"
TITLE_INITIAL = "DOOM"
START_TIMEOUT_S = 40.0


class Result:
    def __init__(self):
        self.passes, self.fails, self.skipped = [], [], False

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def changed_fraction(a, b):
    """Fraction of pixels that differ perceptibly between two frames.

    Thresholded at 12 rather than compared exactly: the status bar's face
    animates and the palette flashes on a pickup, so "identical" is too
    strict for "the player is standing still"."""
    diff = ImageChops.difference(a, b).convert("L")
    changed = sum(1 for p in diff.getdata() if p > 12)
    return changed / float(diff.size[0] * diff.size[1])


def content_box(win):
    c = win["content"]
    return (c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"])


def run(dbg, qmp, tmp, res):
    dbg.send(f"gui spawn {EXEC}")

    win = None
    deadline = time.time() + START_TIMEOUT_S
    while time.time() < deadline:
        win = dbg.window(TITLE_LOADED) or dbg.window(TITLE_INITIAL)
        if win and win.get("content"):
            break
        time.sleep(0.5)

    if not win:
        res.check("DOOM opens a window", False, f"no window within {START_TIMEOUT_S}s")
        return
    res.check("DOOM opens a window", True)

    # A missing IWAD is a SKIP, not a failure. The app says so in its own
    # log, which is the cheapest place to ask.
    logs = " ".join(dbg.logs("doom:", clear=False))
    if "no IWAD" in logs:
        print("\ndoom_test: SKIPPED -- no IWAD on the disk image.")
        print("doom_test: python3 tools/fetch_wad.py && make iso")
        res.skipped = True
        return

    # The game retitles its own window once the WAD is identified. That
    # is DG_SetWindowTitle -> uapp_set_title -> WIN_REQ_TITLE, a round
    # trip the app could not fake.
    deadline = time.time() + START_TIMEOUT_S
    titled = False
    while time.time() < deadline:
        if dbg.window(TITLE_LOADED):
            titled = True
            break
        time.sleep(0.5)
    res.check("the game retitles its own window from the WAD", titled,
              "still called 'DOOM'; DG_SetWindowTitle did not reach TWS")

    win = dbg.window(TITLE_LOADED) or win
    box = content_box(win)

    def shot(tag):
        p = os.path.join(tmp, f"doom_{tag}.png")
        qmp.screenshot(p)
        return Image.open(p).convert("RGB").crop(box)

    # Focus it, or keys go somewhere else and every check below fails for
    # the wrong reason.
    dbg.click((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
    dbg.settle()
    time.sleep(1.0)

    first = shot("0")
    colours = len(first.getcolors(maxcolors=1 << 20) or [])
    res.check("DOOM draws real content", colours > 32,
              f"only {colours} distinct colours -- a blank or flat window")

    # SAMPLED, not compared twice. See the docstring: the title screen is
    # static on purpose, so "any consecutive pair differs" is the claim
    # that does not flake.
    frames = [first]
    for i in range(6):
        time.sleep(0.8)
        frames.append(shot(str(i + 1)))
    moved = any(frames[i].tobytes() != frames[i + 1].tobytes()
                for i in range(len(frames) - 1))
    res.check("DOOM animates", moved,
              "every sampled frame was pixel-identical over ~5s")

    # Escape opens the menu.
    before = frames[-1]
    qmp.send_key("esc")
    time.sleep(2.0)
    after = shot("menu")
    res.check("a keystroke reaches the game (Esc opens the menu)",
              after.tobytes() != before.tobytes(),
              "the screen did not change; input is not reaching the client")

    # --- Enter SELECTS, and the oracle is that the game goes STILL ---
    #
    # THIS CHECK EXISTS BECAUSE THE PORT SHIPPED UNABLE TO START A GAME.
    # `/etc/kbs` maps Enter to 0x0A, because that is what a terminal and
    # a line editor want; `doomkeys.h` defines KEY_ENTER as 0x0D. Nothing
    # crashed and nothing logged: the menu opened, the highlight moved
    # with the arrow keys, and no item could be chosen.
    #
    # **AND THE FIRST VERSION OF THIS CHECK WAS USELESS**, which is the
    # part worth keeping. It pressed Enter and asserted the screen
    # changed -- and the screen changes anyway, because Doom's attract
    # demo keeps playing behind the menu. It passed with the fix
    # reverted. Ask what a broken version would still pass.
    #
    # The oracle that works is the opposite of movement: once a NEW GAME
    # starts, the player is standing still, so the screen goes nearly
    # STATIC -- while a demo, by definition, cannot. Measured, both ways:
    # 0.0009 of pixels changing between frames with a game started,
    # against 0.67 with the fix reverted and the demo still running. The
    # threshold below sits three orders of magnitude clear of both.
    #
    # It pairs with "DOOM animates" above: animating before, still after,
    # is a combination only a working Enter produces.
    qmp.send_key("ret")          # New Game
    time.sleep(1.5)
    qmp.send_key("ret")          # Knee-Deep in the Dead
    time.sleep(1.5)
    qmp.send_key("ret")          # skill level -- the game starts here
    time.sleep(4.0)

    idle_a = shot("game_a")
    time.sleep(1.5)
    idle_b = shot("game_b")
    changed = changed_fraction(idle_a, idle_b)
    print(f"        (frame-to-frame change after New Game: {changed:.4f})")
    res.check("Enter starts a New Game (the screen goes still)",
              changed < 0.05,
              f"{changed:.4f} of pixels still changing -- the attract demo "
              f"is running, so Enter never selected anything. This "
              f"keyboard sends 0x0A and Doom wants KEY_ENTER (0x0D).")

    # --- Ctrl FIRES, which is the other abstract-code trap ------------
    #
    # `key_fire` is KEY_FIRE (0xa3) and `key_use` is KEY_USE (0xa2):
    # ABSTRACT codes that no physical key produces, which Doom expects
    # the platform layer to map onto. The first version of this port
    # mapped Ctrl to KEY_RCTRL and let Space through as 0x20 -- both real
    # Doom key codes, and NEITHER BOUND TO ANYTHING. So the two
    # most-used controls in the game did nothing, silently, while every
    # menu key worked.
    #
    # The oracle is the standing-still one from above, inverted: the game
    # is static when idle, so anything that changes the screen while
    # nobody is moving is the weapon firing. Measured 0.0336 with Ctrl
    # held against a 0.0000 idle baseline.
    idle_c = shot("prefire")
    qmp.key_down("ctrl")
    time.sleep(1.2)
    firing = shot("firing")
    qmp.key_up("ctrl")
    fired = changed_fraction(idle_c, firing)
    print(f"        (frame-to-frame change while firing: {fired:.4f})")
    res.check("Ctrl fires the weapon",
              fired > 0.005,
              f"only {fired:.4f} changed while Ctrl was held -- Ctrl is not "
              f"reaching Doom as KEY_FIRE (0xa3); KEY_RCTRL is bound to "
              f"nothing")

    # --- maximized, and still 4:3 ------------------------------------
    #
    # Doom's pixels are 20% taller than they are wide, so a window that
    # simply stretched 640x400 to fill would show a squashed image. The
    # claim is that the CONTENT stays 4:3 whatever the window does, with
    # black bars taking up the difference -- so this measures the INK,
    # not the window.
    dbg.rclick(win["x"] + win["w"] // 2, win["y"] + 8)
    dbg.settle()
    time.sleep(0.5)
    row = dbg.ctxmenu_row("Maximize")
    if not row:
        res.check("the window menu offers Maximize", False,
                  "no 'Maximize' row -- is the window resizable?")
    else:
        res.check("the window menu offers Maximize", True)
        dbg.click(row[0], row[1])
        dbg.settle()
        time.sleep(3.0)

        big = dbg.window(TITLE_LOADED) or dbg.window(TITLE_INITIAL)
        state = big.get("state")
        res.check("it maximizes", state == "maximized", f"state is {state!r}")

        c = big["content"]
        res.check("the maximized window is bigger than the default",
                  c["w"] > 640 and c["h"] > 480, f"content is {c['w']}x{c['h']}")

        box = content_box(big)
        time.sleep(1.5)
        shot_max = shot("max")
        # The drawn image's extent, found by scanning in from the edges
        # for a non-black column. The bars are exactly black
        # (ugfx_rgb(0,0,0)); Doom's own darkest pixels are not, which is
        # what makes this measurable rather than a guess.
        w_px, h_px = shot_max.size
        mid_y = h_px // 2
        left = 0
        while left < w_px and shot_max.getpixel((left, mid_y)) == (0, 0, 0):
            left += 1
        right = w_px - 1
        while right > left and shot_max.getpixel((right, mid_y)) == (0, 0, 0):
            right -= 1
        ink_w = right - left + 1

        # Expected: the largest 4:3 rect that fits. Tolerance of a few
        # pixels for the integer arithmetic on both sides, and because a
        # genuinely black column at the image's own edge would shave one.
        want = min(w_px, h_px * 4 // 3)
        res.check("the maximized image keeps a 4:3 aspect",
                  abs(ink_w - want) <= 8,
                  f"drawn width {ink_w}px across a {w_px}x{h_px} content "
                  f"area; 4:3 wants {want}px")
        print(f"        (drawn {ink_w}x{h_px} inside {w_px}x{h_px})")

    # The frame rate, from the app's own report. AFTER the maximize, so
    # the number reported is the scaled path's -- which is the one that
    # could plausibly be too slow.
    time.sleep(6.0)
    fps = None
    # "doom: 350 frames, 34.1 fps over the last 5130 ms" -- the LAST such
    # line wins, so the number is the most recent batch rather than the
    # first, which is the one that includes startup.
    for line in dbg.logs("doom:", clear=False):
        if " fps " in line:
            toks = line.split()
            for i, t in enumerate(toks):
                if t == "fps" and i:
                    try:
                        fps = float(toks[i - 1])
                    except ValueError:
                        pass
    if fps is None:
        res.check("DOOM reports a frame rate", False,
                  "no 'N fps' line; the game may not be ticking")
    else:
        print(f"        (measured {fps} fps; Doom's own target is 35)")
        res.check("DOOM runs at a playable rate", fps > 15.0,
                  f"{fps} fps -- the emulator is not keeping up")


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

    if res.skipped:
        return 0
    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\ndoom_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
