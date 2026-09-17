#!/usr/bin/env python3
"""tools/idle_desktop_test.py -- nothing paints over an IDLE desktop.

WHY THIS EXISTS
---------------
A blinking text cursor appeared on top of a desktop icon, and all 23 GUI
tools passed while it was visibly on screen. It was the framebuffer
CONSOLE's cursor: once init started the desktop at boot
(docs/init-design.md stage 2) the physical shell sat at a prompt behind
it, and the shell's idle loop kept calling `vga_cursor_tick()` and
`vga_present()` -- console upkeep, published straight into the
framebuffer the compositor owns.

Every existing tool missed it for the same structural reason: they all
DRIVE the desktop and then assert on what changed. Nothing asked the
opposite question -- with nobody touching it, does the screen sit still?
That is the question here, and it is the cheapest possible check for a
whole class of bug (a second owner writing to the framebuffer) that
otherwise reaches a human before it reaches a test.

WHAT IT ASSERTS
---------------
Two regions must be pixel-IDENTICAL across several captures a third of a
second apart: the desktop's icon column, and an empty patch of desktop.

THE TASKBAR IS DELIBERATELY EXCLUDED. Its clock shows seconds, so it
changes on purpose roughly once a second -- and that is used here rather
than merely avoided: the clock region is asserted to CHANGE, which is
what proves the capture pipeline can see motion at all. Without it, a
harness that returned the same cached image every time would report a
perfectly steady desktop and pass.

POSITIVE CONTROL, verified: in `kernel/drivers/input/keyboard.c`, drop the
`keyboard_blocking_suspended()` guards around `vga_cursor_tick()` and
`vga_present()` in the blocking wait loop, and rebuild. The Control Panel
icon region goes from 1 distinct image in 8 captures to 2, the empty
desktop region stays steady (the cursor sits over an icon, not over open
desktop) and the clock check stays green. So exactly one check fires,
which is the right one.

    python3 tools/vm.py start
    python3 tools/idle_desktop_test.py
    echo $?
"""

import argparse
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, wait_for_desktop  # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"

# How many captures, and how far apart. The console cursor blinks at
# roughly 2 Hz, so 8 captures 0.35 s apart span several full periods --
# enough that a blink cannot hide between samples, which a two-capture
# version could easily let it do.
SAMPLES = 8
INTERVAL_S = 0.35


def settle(qmp, tmp, box, timeout_s=25.0):
    """Wait until the screen stops changing, and say whether it did.

    REQUIRED before sampling, and the reason is this tool's own first
    bug: run inside `gui_regress` it started about five seconds after
    boot, while the desktop was still finishing its first paint, and
    reported BOTH regions changing -- a perfect false positive, since a
    desktop that has not finished drawing yet is not an idle one.

    This is CLAUDE.md's standing rule ("a screendump compared against
    another screendump must be a SETTLED frame") applied to a whole
    region rather than to one window. The taskbar is excluded from the
    comparison because its clock ticks on purpose and would never settle.
    """
    from PIL import Image
    prev = None
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        path = qmp.screenshot(os.path.join(tmp, "settle.png"), stable=False)  # this loop IS the settle
        cur = hashlib.md5(
            Image.open(path).convert("RGB").crop(box).tobytes()).hexdigest()
        if cur == prev:
            return True
        prev = cur
        time.sleep(0.6)
    return False


def region_hashes(qmp, tmp, boxes, interval_s=None):
    """One md5 per box per capture, as {name: [hash, ...]}."""
    from PIL import Image
    out = {name: [] for name in boxes}
    for i in range(SAMPLES):
        path = qmp.screenshot(os.path.join(tmp, f"idle{i}.png"), stable=False)  # measuring motion
        im = Image.open(path).convert("RGB")
        for name, box in boxes.items():
            out[name].append(hashlib.md5(im.crop(box).tobytes()).hexdigest()[:10])
        time.sleep(interval_s if interval_s is not None else INTERVAL_S)
    return out


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"    [{detail}]" if detail else ""))
        (self.passes if ok else self.fails).append(name)


def run(dbg, qmp, tmp, res):
    state = dbg.json("gui state --json") or {}
    screen = state.get("screen") or {}
    w, h = int(screen.get("w", 0)), int(screen.get("h", 0))
    res.check("the desktop is up and answering", w > 0 and h > 0,
              f"{w}x{h}")
    if not w:
        return

    tb = int(state.get("taskbar_h") or 0)

    # Geometry from the WM, not hardcoded: the icon pitch is font-derived
    # (docs/gui-guidelines.md), so a font-size change moves all of this.
    # The icon column starts at the top-left; two icon rows is enough to
    # cover where a console cursor parked at the shell's prompt lands.
    boxes = {
        "the icon column":     (0, 0, 200, 260),
        "empty desktop":       (400, 200, 700, 400),
        # The clock, which MUST tick -- see the module docstring.
        "the taskbar clock":   (max(0, w - 220), max(0, h - tb), w, h),
    }

    # Everything above the taskbar: what must be still, and therefore
    # what has to have stopped moving before sampling starts.
    quiet = (0, 0, w, max(1, h - tb - 1))
    ok = settle(qmp, tmp, quiet)
    # The detail only on FAILURE. Printing it unconditionally made a
    # PASSING run report "still changing after 25s", which is the kind of
    # output that gets a real failure dismissed as noise later.
    res.check("the desktop settles into a steady frame", ok,
              "" if ok else "still changing after 25s -- something is painting")

    # NOTE THIS IS A PRECONDITION, NOT THE ASSERTION. settle() compares
    # two captures 0.6s apart, and a ~2 Hz blink can present the same
    # phase to both -- verified: with the console cursor deliberately
    # left blinking, this check PASSES and the region checks below are
    # what catch it. Do not be tempted to lean on it.

    seen = region_hashes(qmp, tmp, boxes)

    for name in ("the icon column", "empty desktop"):
        distinct = sorted(set(seen[name]))
        res.check(f"{name} is steady while nothing touches the desktop",
                  len(distinct) == 1,
                  f"{len(distinct)} distinct in {SAMPLES}: {distinct}")

    # The control. If this one fails, the two above prove NOTHING -- a
    # harness handing back one cached frame would pass them both.
    #
    # RETRIED OVER A LONGER WINDOW before failing: under the full
    # suite's parallelism a starved guest can genuinely not repaint the
    # once-a-second clock inside SAMPLES x INTERVAL_S (~3s) -- measured
    # twice in one day as the suite's only red check, passing solo both
    # times. A truly frozen capture pipeline still fails the slow pass.
    clock = sorted(set(seen["the taskbar clock"]))
    if len(clock) <= 1:
        slow = region_hashes(qmp, tmp,
                              {"the taskbar clock": boxes["the taskbar clock"]},
                              interval_s=1.2)
        clock = sorted(set(slow["the taskbar clock"]))
    res.check("the taskbar clock DOES change (proves motion is visible)",
              len(clock) > 1,
              f"{len(clock)} distinct in {SAMPLES}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "idle_desktop_test")

    # WAIT FOR THE DESKTOP BEFORE ASKING IT ANYTHING. This tool was the
    # only one that connected and queried straight away -- every other
    # goes through enter_gui(), which polls. init starts the desktop at
    # boot, so it is usually up before a tool connects, and "usually" is
    # what a flake is made of: `gui state --json` answered "no window
    # manager running" 1 run in 6 at HEAD and 3 in 6 on a busier tree,
    # two seconds in. It does NOT use enter_gui(), which also turns on
    # the per-frame layout log -- this tool measures what an IDLE
    # desktop does, and giving it more to log is the last thing it wants.
    wait_for_desktop(args.sock)

    qmp = QMPSession(port=args.qmp_port)
    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, res)
    finally:
        dbg.close()

    print(f"\nidle_desktop_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
