#!/usr/bin/env python3
"""A desktop above 1280x720, and a client window that can actually fill it.

WHAT IS UNDER TEST
------------------
Two constants that have to move together and did not:

  * the mode the display layer selects (DISPLAY_MAX_W/H plus the ladder
    in kernel/drivers/display/display.c, and whether a modesetting
    driver -- bochs.c, vmsvga.c, virtio-gpu -- picked it up), and
  * WIN_CLIENT_MAX_W/H (kernel/include/abi/win_proto.h), the largest
    pixel buffer the window server will hand a ring-3 client.

When the second is smaller than the first, MAXIMIZE FAILS SILENTLY. The
WM proposes the new content size, `resize_window()` refuses it (a
refusal is a normal protocol outcome -- it looks exactly like a client
declining), the client keeps its old buffer, and the window wears
full-screen chrome around it with undrawn desktop filling the
difference. Nothing logs an error and no existing GUI tool notices,
because every one of them runs at the mode this OS boots into by
default.

HOW IT IS ASSERTED, AND WHY IN PIXELS
-------------------------------------
`gui windows --json` reporting w=1920 is good evidence but not proof:
the WM adopts a client's new size only when the client acks the resize,
so the number is second-hand. The load-bearing check is a PIXEL far
outside any 1280x720 buffer -- it is the desktop's own background colour
before the window is maximized and must be the client's afterwards.
Sampled with the neighbour rule this repo insists on: a taskbar pixel is
sampled at the same time and must NOT change, so "the client painted
everywhere" cannot pass by the whole screen having been repainted.

THE FIXTURE HAS TO REACH THE BRANCH
-----------------------------------
At 1280x720 every check here passes trivially -- the far-corner sample
lands inside a buffer the old cap already allowed, and the test measures
nothing. That is exactly this repo's "the data never reached the code
under test" trap, so --require-min (default 1920x1080) FAILS rather than
skips when the guest is not actually running big enough.

Getting the guest there: the mode is chosen at boot, so it comes from
the ISO's kernel command line.

    make iso KCMDLINE="video=1920x1080"     # re-seeds disk.img
    python3 tools/vm.py start
    python3 tools/hires_test.py
    python3 tools/vm.py stop
    make iso                                # put the default ISO back

Not in gui_regress.py or preflight.sh for that reason -- it needs an ISO
built differently from the one every other tool wants. It is the check
to run when touching display modes, the window-buffer caps, gfx.c's back
buffer, or anything that assumes a screen size.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
APP = "Notepad"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def pixels(qmp, path, points):
    """Sample several points out of one settled screendump."""
    from PIL import Image
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    return [im.getpixel(p) for p in points]


def ctx_action(dbg, win, label):
    """Right-click the title bar and pick a menu row BY LABEL.

    By label rather than by a computed row index, and on the title bar
    rather than on the maximize button's pixel rect, so nothing here
    needs re-measuring when the font or the chrome height changes.
    """
    dbg.rclick(win["x"] + win["w"] // 2, win["y"] + 6)
    dbg.settle()
    row = dbg.ctxmenu_row(label)
    if row is None:
        return False
    dbg.click(*row)
    dbg.settle()
    time.sleep(1.0)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--require-min", default="1920x1080",
                    help="fail unless the guest screen is at least this big")
    ap.add_argument("--shots", default=None,
                    help="directory to keep the screendumps in")
    args = ap.parse_args()

    shots = args.shots or os.path.join(os.getcwd(), ".hires_shots")
    os.makedirs(shots, exist_ok=True)

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.5)

    dbg = DebugConsole(args.sock)
    print("hi-res desktop (display mode vs WIN_CLIENT_MAX_W/H)")

    state = dbg.state()
    sw, sh = state["screen"]["w"], state["screen"]["h"]
    tb = state["taskbar_h"]
    print(f"  screen {sw}x{sh}, taskbar {tb}px")

    min_w, _, min_h = args.require_min.partition("x")
    big_enough = sw >= int(min_w) and sh >= int(min_h)
    if not check("the guest is running big enough to test anything", big_enough,
                 "" if big_enough else
                 f"{sw}x{sh} < {args.require_min} -- rebuild the ISO with "
                 f'KCMDLINE="video={args.require_min}"'):
        print("\nevery check below would pass vacuously at this size")
        return report()

    # The sample point: inside a maximized window, and far outside any
    # 1280x720 buffer measured from the window's origin. The control
    # point sits in the taskbar, which no client may ever paint.
    sample = (sw - 20, sh - tb - 20)
    control = (sw // 2, sh - tb // 2)

    dbg.open_app(APP)
    dbg.settle()
    time.sleep(1.2)
    wins = [w for w in dbg.windows() if w["state"] != "minimized"]
    if not check(f"{APP} opened", len(wins) == 1, f"{len(wins)} window(s)"):
        return report()
    win = wins[-1]
    check("...and does not already cover the sample point",
          win["x"] + win["w"] < sample[0] or win["y"] + win["h"] < sample[1],
          f"window {win['w']}x{win['h']} at {win['x']},{win['y']}")

    before_sample, before_control = pixels(qmp, os.path.join(shots, "before.png"),
                                            [sample, control])

    if not check("the title-bar context menu offers Maximize",
                 ctx_action(dbg, win, "Maximize")):
        return report()

    wins = [w for w in dbg.windows() if w["title"] == win["title"]]
    if not check("the window is still there after maximizing", bool(wins)):
        return report()
    m = wins[-1]
    check("the WM calls it maximized", m["state"] == "maximized", m["state"])
    check("the FRAME fills the screen above the taskbar",
          m["w"] == sw and m["h"] == sh - tb,
          f"{m['w']}x{m['h']}, expected {sw}x{sh - tb}")
    # Second-hand, but it is the number the client itself acked: the WM
    # adopts a client window's size only from on_window_resized().
    check("the CLIENT's content rect is bigger than the old 1280x720 cap",
          m["content"]["w"] > 1280 and m["content"]["h"] > 720,
          f"content {m['content']['w']}x{m['content']['h']}")

    after_sample, after_control = pixels(qmp, os.path.join(shots, "after.png"),
                                          [sample, control])

    # THE CHECK THAT CANNOT BE PASSED BY A SILENTLY-REFUSED RESIZE: the
    # far corner used to be desktop and must now be the client's pixels.
    check("the client PAINTED the far corner (not the desktop showing through)",
          after_sample != before_sample,
          f"{sample}: {before_sample} -> {after_sample}")
    check("...and the taskbar underneath is untouched",
          after_control == before_control,
          f"{control}: {before_control} -> {after_control}")

    # Restoring has to work too: the same protocol runs in reverse, and a
    # cap that refused the shrink would leave a full-screen buffer inside
    # a small frame -- the same bug pointing the other way.
    if check("the context menu offers Restore", ctx_action(dbg, m, "Restore")
             or ctx_action(dbg, m, "Maximize")):
        wins = [w for w in dbg.windows() if w["title"] == win["title"]]
        r = wins[-1] if wins else None
        check("restoring puts it back to a normal window",
              r is not None and r["state"] == "normal" and r["w"] < sw,
              f"{r['state']} {r['w']}x{r['h']}" if r else "gone")
        back_sample, _ = pixels(qmp, os.path.join(shots, "restored.png"), [sample, control])
        check("...and the far corner is desktop again",
              back_sample == before_sample,
              f"{sample}: {back_sample} vs {before_sample}")

    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nhires_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
