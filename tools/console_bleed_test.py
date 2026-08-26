#!/usr/bin/env python3
"""The kernel console must not paint over the desktop -- and must not lose the text.

A ring-3 process writing to fd 1 reaches the framebuffer console, which
draws into its own back buffer and then BLITS THAT OVER THE WHOLE SCREEN.
While the desktop owns the screen that is a full-screen text console
flashing over it, and it is not the writing program's fault: `dmesg`
covered 100% of the desktop, and DOOM -- which prints a startup banner --
was how it got noticed.

The fix is Linux's KD_GRAPHICS: vga_present() does nothing while a
compositor holds the screen, the console keeps buffering, and
vga_resume() repaints on the way out. So there are TWO properties here
and testing either alone is worthless:

  * nothing bleeds through while the desktop is up, and
  * the text is still there afterwards.

A version that simply stopped the console drawing would pass the first
and fail the second, which is why the console screen is captured BEFORE
the desktop starts and compared against the one after `Exit to shell`:
the console must have CHANGED, because output happened while it was
invisible. Comparing against "is there any text" would pass vacuously --
the boot log is already on that screen.

The bleed check needs its own control, for the reason
idle_desktop_test.py exists: a harness returning one cached frame
reports a beautifully clean desktop. So a real window is opened
afterwards and the screen MUST change.

Usage (this tool boots and drives its own guest):
    python3 tools/console_bleed_test.py
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
# The loudest thing in /bin: the whole kernel ring buffer, hundreds of
# lines. A short write may never cross a PIT tick and so may never
# present at all, which would make this pass without the fix.
NOISY = "/bin/dmesg"

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def shot(qmp, tmp, tag):
    p = os.path.join(tmp, f"{tag}.png")
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def diff_pct(a, b, skip_bottom=0):
    """Percentage of sampled pixels that differ.

    `skip_bottom` excludes the taskbar, whose clock changes once a second
    on its own -- a difference that is real and has nothing to do with
    what is being measured.
    """
    w, h = a.size
    lim = h - skip_bottom
    n = diff = 0
    for y in range(0, lim, 4):
        for x in range(0, w, 4):
            n += 1
            if a.getpixel((x, y)) != b.getpixel((x, y)):
                diff += 1
    return 100.0 * diff / max(n, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="bleed_")
    dbg = DebugConsole(args.sock)
    qmp = QMPSession(port=args.qmp_port)

    print("the console must not paint over the desktop")

    # --- the console BEFORE the desktop exists ------------------------
    # Kept for the last check. Taken now because after this point the
    # console is never on screen again until the desktop is dismissed.
    dbg.send("")
    time.sleep(0.5)
    console_before = shot(qmp, tmp, "console_before")

    enter_gui(qmp, args.sock)
    dbg.settle()
    # The wallpaper is a photograph and the version string sits over it;
    # neither moves, so neither matters here. What does move is the
    # taskbar clock, which is why every comparison skips those rows.
    bar = dbg.taskbar()
    skip = bar["h"] + 2
    desktop = shot(qmp, tmp, "desktop")

    # --- 1. a very noisy program must not reach the screen ------------
    dbg.send(f"gui spawn {NOISY}")
    peak = 0.0
    worst = None
    for i in range(20):
        f = shot(qmp, tmp, f"noisy_{i:02d}")
        d = diff_pct(desktop, f, skip_bottom=skip)
        if d > peak:
            peak, worst = d, i
        time.sleep(0.12)
    check("a program dumping the kernel log does not reach the desktop",
          peak < 1.0, f"peak {peak:.1f}% (frame {worst})")

    # --- 2. ...and the harness could have seen it if it had ------------
    # Without this the check above passes on a dead capture pipeline, a
    # frozen guest, or a screendump that returns the same cached frame.
    dbg.settle()
    dbg.open_app("Calculator")
    dbg.settle()
    opened = shot(qmp, tmp, "calculator")
    moved = diff_pct(desktop, opened, skip_bottom=skip)
    check("...and the same capture DOES see a real window open",
          moved > 2.0, f"{moved:.1f}% changed")

    # --- 3. the text was kept, not thrown away ------------------------
    # The desktop goes away, and the console must show what was printed
    # while it was invisible. Compared against the console as it was BEFORE the
    # desktop started: equal would mean the output was dropped, and a
    # blank screen would mean the repaint cleared instead of restoring.
    # TAKE THE DESKTOP OUT OF INIT'S HANDS FIRST. Init restarts it with a
    # zero backoff and REUSES the slot, so without this the console comes
    # back for a few frames and a new desktop covers it again -- the pid
    # does not even change. Same precondition compositor_death_test.py
    # establishes, and for the same reason. `make iso` re-seeds /etc by
    # sync, so the file comes back on the next build -- there is nothing
    # in the guest to copy it from, which is why this is not restored
    # here. IT LEAVES THE IMAGE WITHOUT AN AUTOSTARTED DESKTOP until the
    # next `make iso`; `enter_gui()` still works, because that runs `gui`
    # by hand. Same trade compositor_death_test.py makes.
    dbg.send("sh rm /etc/services.d/toywm")
    time.sleep(0.5)
    # KILLED rather than asked through the Start menu. Both land in
    # win_server.c's compositor_gone(), which is the path under test, and
    # a kill needs no menu row to be found, hovered and hit -- one less
    # thing that can fail for a reason this tool is not about.
    procs = dbg.processes_named("toywm")
    check("the desktop process is there to stop", bool(procs),
          f"found {[p['name'] for p in procs]}")
    if not procs:
        return 1
    dbg.send(f"sh kill {procs[0]['pid']}")

    # WAIT FOR IT TO ACTUALLY LET GO, and prove it did. The first version
    # clicked a menu row that was never open, then compared the pre-GUI
    # console against a still-running desktop and PASSED -- those two
    # differ by 6.5% for reasons that have nothing to do with this.
    gone = False
    for _ in range(40):
        time.sleep(0.5)
        if not dbg.processes_named("toywm"):
            gone = True
            break
    check("stopping the desktop actually released the screen", gone,
          "released" if gone
          else "still running -- the check below would be vacuous")
    if not gone:
        return 1

    time.sleep(1.5)
    console_after = shot(qmp, tmp, "console_after")
    changed = diff_pct(console_before, console_after)
    check("the text printed while the desktop was up survives its return",
          changed > 5.0,
          f"{changed:.1f}% of the console changed (0% would mean it was dropped)")

    failed = [n for n, ok in checks if not ok]
    print(f"\n{len(checks) - len(failed)}/{len(checks)} checks passed"
          + (f" -- FAILED: {', '.join(failed)}" if failed else ""))
    print(f"frames in {tmp}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
